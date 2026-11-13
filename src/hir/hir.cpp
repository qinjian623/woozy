#include "lang/hir/hir.hpp"
#include "constant_value.hpp"
#include "foreign.hpp"
#include "literal_value.hpp"
#include "ownership.hpp"
#include "pattern_analysis.hpp"
#include "public_type_visibility.hpp"
#include <algorithm>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <type_traits>

namespace lang::hir {
namespace {
bool integer(TypeKind k) {
  return k == TypeKind::SignedInt || k == TypeKind::UnsignedInt ||
         k == TypeKind::Isize || k == TypeKind::Usize;
}
bool signed_integer(TypeKind k) {
  return k == TypeKind::SignedInt || k == TypeKind::Isize;
}
bool number(TypeKind k) { return integer(k) || k == TypeKind::Float; }
bool inout_scalar(TypeKind k) { return number(k) || k == TypeKind::Bool; }
bool mode_valid(ParamMode m) {
  return m == ParamMode::ordinary || m == ParamMode::inout ||
         m == ParamMode::move;
}
bool nominal(TypeKind k) {
  return k == TypeKind::NominalStruct || k == TypeKind::NominalEnum ||
         k == TypeKind::TypeParameter;
}
bool foreign_access_read(const HirNode &n) {
  const auto *r = std::get_if<ForeignRead>(&n.data);
  return r && r->access &&
         (*r->access == ForeignAccessKind::property_read ||
          *r->access == ForeignAccessKind::index_read ||
          *r->access == ForeignAccessKind::slice_read);
}
bool foreign_access_place(const HirNode &n) {
  const auto *r = std::get_if<ForeignRead>(&n.data);
  return r && r->access &&
         (*r->access == ForeignAccessKind::property_place ||
          *r->access == ForeignAccessKind::index_place ||
          *r->access == ForeignAccessKind::slice_place);
}
bool type_contains_parameter(const HirModule &m, TypeId id) {
  std::vector<TypeId> pending{id};
  std::set<uint64_t> seen;
  while (!pending.empty()) {
    auto current = pending.back();
    pending.pop_back();
    if (!seen.insert(current.value).second)
      continue;
    const auto *t = m.types.find(current);
    if (!t)
      return false;
    if (t->kind == TypeKind::TypeParameter)
      return true;
    pending.insert(pending.end(), t->type_args.begin(), t->type_args.end());
    for (auto p : t->parameters)
      pending.push_back(p.type);
    if (t->kind == TypeKind::Function) {
      pending.push_back(t->result);
      if (t->throws_type)
        pending.push_back(*t->throws_type);
    }
  }
  return false;
}
} // namespace
const Type *TypeContext::find(TypeId id) const {
  return id.value && id.value <= entries_.size() ? &entries_[id.value - 1]
                                                 : nullptr;
}
TypeId TypeContext::intern(Type t) {
  auto bad = [] {
    throw std::invalid_argument("invalid canonical type shape/reference");
  };
  switch (t.kind) {
  case TypeKind::SignedInt:
  case TypeKind::UnsignedInt:
    if (t.bits != 8 && t.bits != 16 && t.bits != 32 && t.bits != 64)
      bad();
    break;
  case TypeKind::Float:
    if (t.bits != 32 && t.bits != 64)
      bad();
    break;
  case TypeKind::Vec:
  case TypeKind::Slice:
    if (t.type_args.size() != 1)
      bad();
    break;
  // Weak[T] is part of the published reference-values schema, but no source
  // expression can construct it yet. Keep the shape check here; HIR
  // verification rejects the kind itself.
  case TypeKind::Weak:
    if (t.type_args.size() != 1)
      bad();
    break;
  case TypeKind::Task:
    if (t.type_args.size() != 2)
      bad();
    break;
  case TypeKind::TraitObject:
    // Trait argument arity is checked against the real declaration by HIR
    // verification. Interning only enforces the structural fat-pointer shape.
    if (!t.declaration.value || t.bits || t.binding.value || t.c_type ||
        t.parameters.size() || t.result.value || t.throws_type)
      bad();
    break;
  case TypeKind::CForeignPointer:
  case TypeKind::CForeignBuffer:
    if (t.type_args.size() != 1 || t.bits > 1 || t.c_type)
      bad();
    break;
  case TypeKind::CForeignRecord:
  case TypeKind::CForeignResource:
  case TypeKind::CForeignCallback:
  case TypeKind::CForeignOpaque:
    if (!t.c_type || !t.type_args.empty() || t.bits)
      bad();
    break;
  case TypeKind::Function:
    if (!find(t.result) || (t.throws_type && !find(*t.throws_type)))
      bad();
    for (auto p : t.parameters)
      if (!find(p.type) || !mode_valid(p.mode))
        bad();
    break;
  case TypeKind::Unit:
  case TypeKind::Bool:
  case TypeKind::String:
  case TypeKind::Isize:
  case TypeKind::Usize:
  case TypeKind::NominalStruct:
  case TypeKind::NominalEnum:
  case TypeKind::TypeParameter:
  case TypeKind::ForeignOpaque:
  case TypeKind::ForeignCallable:
  case TypeKind::ThreadHandle:
  case TypeKind::MutexHandle:
  case TypeKind::CondvarHandle:
  case TypeKind::AtomicI64Handle:
  case TypeKind::IpcHandle:
  case TypeKind::Error:
    break;
  default:
    bad();
  }
  if (t.kind != TypeKind::Function &&
      (!t.parameters.empty() || t.result.value || t.throws_type))
    bad();
  if (t.kind != TypeKind::SignedInt && t.kind != TypeKind::UnsignedInt &&
      t.kind != TypeKind::Float && t.kind != TypeKind::CForeignPointer &&
      t.kind != TypeKind::CForeignBuffer && t.bits)
    bad();
  if (!nominal(t.kind) && t.kind != TypeKind::TraitObject &&
      t.kind != TypeKind::ForeignOpaque &&
      t.kind != TypeKind::ForeignCallable &&
      (t.declaration.value || t.binding.value))
    bad();
  if (nominal(t.kind) && t.binding.value)
    bad();
  if (t.kind != TypeKind::NominalStruct && t.kind != TypeKind::NominalEnum &&
      t.kind != TypeKind::Vec && t.kind != TypeKind::Slice &&
      t.kind != TypeKind::Weak && t.kind != TypeKind::Task &&
      t.kind != TypeKind::TraitObject &&
      t.kind != TypeKind::CForeignPointer &&
      t.kind != TypeKind::CForeignBuffer && !t.type_args.empty())
    bad();
  if ((t.kind == TypeKind::CForeignRecord ||
       t.kind == TypeKind::CForeignResource ||
       t.kind == TypeKind::CForeignCallback ||
       t.kind == TypeKind::CForeignOpaque) != bool(t.c_type))
    bad();
  for (auto a : t.type_args)
    if (!find(a))
      bad();
  for (size_t i = 0; i < entries_.size(); ++i)
    if (entries_[i] == t)
      return TypeId{i + 1};
  entries_.push_back(std::move(t));
  return TypeId{entries_.size()};
}
TypeId substitute(TypeId id, const Substitutions &sub, TypeContext &ctx) {
  std::map<uint64_t, TypeId> cache;
  std::function<TypeId(TypeId)> visit = [&](TypeId current) {
    if (auto it = cache.find(current.value); it != cache.end())
      return it->second;
    const auto *found = ctx.find(current);
    if (!found)
      throw std::invalid_argument("substitution missing type");
    auto t = *found;
    if (t.kind == TypeKind::TypeParameter) {
      for (auto [s, value] : sub)
        if (s == t.declaration)
          return cache[current.value] = value;
      return cache[current.value] = current;
    }
    for (auto &a : t.type_args)
      a = visit(a);
    for (auto &p : t.parameters)
      p.type = visit(p.type);
    if (t.kind == TypeKind::Function) {
      t.result = visit(t.result);
      if (t.throws_type)
        t.throws_type = visit(*t.throws_type);
    }
    return cache[current.value] = ctx.intern(std::move(t));
  };
  return visit(id);
}
bool type_patterns_overlap(TypeId a, TypeId b, const TypeContext &ctx) {
  std::map<uint64_t, TypeId> bindings;
  auto type = [&](TypeId id) -> const Type & {
    auto t = ctx.find(id);
    if (!t)
      throw std::invalid_argument("overlap type reference");
    return *t;
  };
  auto dereference = [&](TypeId id) {
    for (size_t i = 0; type(id).kind == TypeKind::TypeParameter; ++i) {
      if (i > bindings.size())
        throw std::invalid_argument("cyclic type pattern bindings");
      auto it = bindings.find(type(id).declaration.value);
      if (it == bindings.end())
        break;
      id = it->second;
    }
    return id;
  };
  auto occurs = [&](SymbolId variable, TypeId root) {
    std::vector<TypeId> pending{root};
    std::set<uint64_t> visited;
    while (!pending.empty()) {
      auto id = dereference(pending.back());
      pending.pop_back();
      if (!visited.insert(id.value).second)
        continue;
      const auto &t = type(id);
      if (t.kind == TypeKind::TypeParameter && t.declaration == variable)
        return true;
      pending.insert(pending.end(), t.type_args.begin(), t.type_args.end());
      for (auto p : t.parameters)
        pending.push_back(p.type);
      if (t.kind == TypeKind::Function) {
        pending.push_back(t.result);
        if (t.throws_type)
          pending.push_back(*t.throws_type);
      }
    }
    return false;
  };
  std::vector<std::pair<TypeId, TypeId>> pending{{a, b}};
  while (!pending.empty()) {
    auto [x, y] = pending.back();
    pending.pop_back();
    x = dereference(x);
    y = dereference(y);
    if (x == y)
      continue;
    const auto &tx = type(x), &ty = type(y);
    if (tx.kind == TypeKind::TypeParameter) {
      if (occurs(tx.declaration, y))
        return false;
      bindings[tx.declaration.value] = y;
      continue;
    }
    if (ty.kind == TypeKind::TypeParameter) {
      if (occurs(ty.declaration, x))
        return false;
      bindings[ty.declaration.value] = x;
      continue;
    }
    if (tx.kind != ty.kind || tx.bits != ty.bits ||
        tx.declaration != ty.declaration || tx.binding != ty.binding ||
        tx.parameters.size() != ty.parameters.size() ||
        tx.type_args.size() != ty.type_args.size() ||
        tx.throws_type.has_value() != ty.throws_type.has_value())
      return false;
    for (size_t i = 0; i < tx.type_args.size(); ++i)
      pending.push_back({tx.type_args[i], ty.type_args[i]});
    if (tx.kind == TypeKind::Function) {
      pending.push_back({tx.result, ty.result});
      if (tx.throws_type)
        pending.push_back({*tx.throws_type, *ty.throws_type});
      for (size_t i = 0; i < tx.parameters.size(); ++i) {
        if (tx.parameters[i].mode != ty.parameters[i].mode)
          return false;
        pending.push_back({tx.parameters[i].type, ty.parameters[i].type});
      }
    }
  }
  return true;
}
std::optional<SymbolId> infinite_value_layout(const HirModule &m,
                                              std::stop_token stop) {
  // A value layout depends on a subset of its type arguments. Find that
  // subset from declarations, without substituting or interning new types.
  // For example Box[T]{x T} uses T, while F[T]{f func() T} does not.
  std::map<uint64_t, std::set<size_t>> used;
  for (const auto &d : m.nominals)
    used.emplace(d.symbol.value, std::set<size_t>{});
  auto walk = [&](TypeId root, const auto &visit) {
    std::vector<TypeId> pending{root};
    std::set<uint64_t> seen;
    while (!pending.empty()) {
      check_cancelled(stop);
      auto id = pending.back();
      pending.pop_back();
      if (!seen.insert(id.value).second)
        continue;
      auto t = m.types.find(id);
      if (!t)
        throw std::invalid_argument("layout has invalid type");
      if (t->kind == TypeKind::TypeParameter)
        visit(*t);
      if (t->kind != TypeKind::NominalStruct &&
          t->kind != TypeKind::NominalEnum)
        continue;
      visit(*t);
      for (auto i : used.at(t->declaration.value))
        pending.push_back(t->type_args.at(i));
    }
  };
  auto fields = [&](const NominalDecl &d, const auto &visit) {
    for (const auto &f : d.fields)
      walk(f.type, visit);
    for (const auto &v : d.variants)
      for (const auto &f : v.payload)
        walk(f.type, visit);
  };
  bool changed;
  do {
    changed = false;
    for (const auto &d : m.nominals)
      fields(d, [&](const Type &t) {
        if (t.kind == TypeKind::TypeParameter)
          for (size_t i = 0; i < d.params.size(); ++i)
            if (t.declaration == d.params[i].symbol)
              changed |= used.at(d.symbol.value).insert(i).second;
      });
  } while (changed);

  // Only demanded argument types contribute declaration dependencies.
  // Removing all finite leaves leaves exactly cycles and their dependents.
  std::map<uint64_t, std::set<uint64_t>> dependencies, dependents;
  std::vector<uint64_t> leaves;
  for (const auto &d : m.nominals) {
    auto &deps = dependencies[d.symbol.value];
    fields(d, [&](const Type &t) {
      if (t.kind == TypeKind::NominalStruct || t.kind == TypeKind::NominalEnum)
        deps.insert(t.declaration.value);
    });
    for (auto target : deps)
      dependents[target].insert(d.symbol.value);
    if (deps.empty())
      leaves.push_back(d.symbol.value);
  }
  while (!leaves.empty()) {
    check_cancelled(stop);
    auto finite = leaves.back();
    leaves.pop_back();
    for (auto parent : dependents[finite]) {
      auto &deps = dependencies.at(parent);
      deps.erase(finite);
      if (deps.empty())
        leaves.push_back(parent);
    }
  }
  for (const auto &d : m.nominals)
    if (!dependencies.at(d.symbol.value).empty())
      return d.symbol;
  return {};
}
unsigned bit_width(const Type &t, const Config &c) {
  if (c.word_bits != 32 && c.word_bits != 64)
    throw std::invalid_argument("target word_bits");
  if (t.kind == TypeKind::Isize || t.kind == TypeKind::Usize)
    return c.word_bits;
  if (integer(t.kind) || t.kind == TypeKind::Float)
    return t.bits;
  throw std::invalid_argument("type has no numeric width");
}
namespace {
struct Invalid {
  std::string message;
  std::optional<SourceSpan> span;
};
class Verifier {
  const HirModule &m;
  std::stop_token stop;
  std::map<uint64_t, const HirFunction *> functions;
  struct Local {
    TypeId type;
    ScopeId scope;
    SymbolId function;
    bool writable;
    size_t ready;
    bool read_only = false;
  };
  std::map<uint64_t, Local> locals;
  std::vector<size_t> enter, leave;
  std::vector<std::optional<SymbolId>> owners;
  std::vector<bool> scope_used;
  std::vector<std::optional<HirNodeId>> parents, scope_nodes;
  size_t clock = 0;
  std::map<uint64_t, const FunctionTemplate *> templates;
  std::map<uint64_t, const NominalDecl *> nominals;
  std::map<uint64_t, const FunctionInstance *> instances;
  std::vector<const HirFunction *> all_functions;
  mutable TypeContext substituted;
  mutable std::map<std::tuple<uint64_t, uint64_t, std::vector<uint64_t>>,
                   bool>
      trait_results;
  mutable std::set<std::tuple<uint64_t, uint64_t, std::vector<uint64_t>>>
      active_constraints;
  mutable std::set<std::tuple<uint64_t, uint64_t, std::vector<uint64_t>>>
      active_trait_bounds;
  mutable std::set<std::pair<uint64_t, std::set<uint64_t>>> validated_types;

  [[noreturn]] void fail(std::string message,
                         std::optional<SourceSpan> span = {}) const {
    throw Invalid{std::move(message), span};
  }
  void need(bool b, std::string_view message) const {
    if (!b)
      fail(std::string(message));
  }
  const Type &type(TypeId id) const {
    auto t = m.types.find(id);
    if (!t)
      fail("missing or out-of-range TypeId");
    return *t;
  }
  const HirNode &node(HirNodeId id) const {
    if (id.value >= m.nodes.size())
      fail("out-of-range HirNodeId");
    return m.nodes[id.value];
  }
  const Scope &scope(ScopeId id) const {
    if (id.value >= m.scopes.size())
      fail("out-of-range ScopeId");
    return m.scopes[id.value];
  }
  const modules::Symbol &symbol(SymbolId id) const {
    if (id.value >= m.resolved->symbols.size())
      fail("out-of-range SymbolId");
    const auto &s = m.resolved->symbols[id.value];
    need(s.id == id, "symbol index/identity mismatch");
    need(s.module.value < m.resolved->project->modules.size(),
         "symbol module index");
    return s;
  }
  void source(const Source &s) const {
    const auto &p = *m.resolved->project;
    need(s.node.module.value < p.modules.size(), "source module index");
    const auto &mod = p.modules[s.node.module.value];
    need(mod.id == s.node.module, "module index/identity mismatch");
    need(s.node.file < mod.files.size() && mod.files[s.node.file],
         "source file index");
    const auto &file = *mod.files[s.node.file];
    need(!file.recovered, "owning AST contains syntax recovery");
    need(file.source && file.source->valid(),
         "source snapshot missing/invalid");
    need(s.node.node.value < file.nodes.size(), "source AST node index");
    const auto &ast = file.nodes[s.node.node.value];
    need(ast.span.begin_byte <= ast.span.end_byte &&
             ast.span.end_byte <= file.source->original.size(),
         "invalid AST source span");
    need(!ast.recovered && ast.kind != AstKind::Error && true,
         "unresolved/recovered AST source");
    need(s.span.snapshot == file.source->id &&
             s.span.snapshot == ast.span.snapshot &&
             s.span.begin_byte <= s.span.end_byte &&
             s.span.end_byte <= file.source->original.size() &&
             s.span.begin_byte >= ast.span.begin_byte &&
             s.span.end_byte <= ast.span.end_byte,
         "invalid source span");
  }
  bool ancestor(ScopeId a, ScopeId b) const {
    for (size_t count = 0; count <= m.scopes.size(); ++count) {
      if (a == b)
        return true;
      auto parent = scope(b).parent;
      if (!parent)
        return false;
      b = *parent;
    }
    fail("scope cycle");
  }
  void unit(const HirNode &n) const {
    need(type(n.type).kind == TypeKind::Unit, "statement must have Unit type");
  }
  bool expression(const HirNode &n) const {
    switch (n.kind()) {
    case NodeKind::unit_literal:
    case NodeKind::constant_literal:
    case NodeKind::bool_literal:
    case NodeKind::integer_literal:
    case NodeKind::float_literal:
    case NodeKind::string_literal:
    case NodeKind::read:
    case NodeKind::function_value:
    case NodeKind::unary:
    case NodeKind::binary:
    case NodeKind::short_circuit:
    case NodeKind::integer_convert:
    case NodeKind::call:
    case NodeKind::struct_init:
    case NodeKind::enum_init:
    case NodeKind::field_read:
    case NodeKind::constraint_call:
    case NodeKind::move:
    case NodeKind::vec_init:
    case NodeKind::index_read:
    case NodeKind::vec_slice:
    case NodeKind::foreign_read:
    case NodeKind::c_field_read:
    case NodeKind::await_:
    case NodeKind::trait_object_init:
    case NodeKind::trait_dispatch:
      return foreign_access_read(n) || !foreign_access_place(n);
    default:
      return false;
    }
  }
  const HirNode &expr(HirNodeId id) const {
    const auto &n = node(id);
    need(expression(n), "expected value expression");
    return n;
  }
  bool statement(const HirNode &n) const {
    return !expression(n) && n.kind() != NodeKind::place &&
           n.kind() != NodeKind::field_place &&
           n.kind() != NodeKind::index_place &&
           !foreign_access_place(n);
  }
  const HirFunction &function(SymbolId s) const {
    symbol(s);
    auto it = functions.find(s.value);
    if (it == functions.end())
      fail("function identity has no HIR definition");
    return *it->second;
  }
  void valid_type(TypeId id, const std::set<uint64_t> &allowed,
                  size_t depth = 0) const {
    need(depth < 128, "type nesting exceeds verification budget");
    if (validated_types.count({id.value, allowed}))
      return;
    const auto &t = type(id);
    need(t.kind != TypeKind::Error, "invalid/unimplemented type");
    if (t.kind == TypeKind::ForeignOpaque || t.kind == TypeKind::ForeignCallable) {
      need(!t.declaration.value && t.binding.value < m.foreign_bindings.size(),
           "foreign type binding index");
      const auto &d = m.resolved->project->foreign_binding(t.binding);
      need(t.kind == TypeKind::ForeignOpaque
               ? std::holds_alternative<modules::BindingType>(d.detail)
               : std::holds_alternative<modules::BindingCallable>(d.detail),
           "foreign type declaration category");
    }
    if (t.kind == TypeKind::CForeignPointer || t.kind == TypeKind::CForeignBuffer) {
      need(t.type_args.size() == 1 && t.bits <= 1 && !t.c_type &&
               !t.declaration.value && !t.binding.value,
           "C pointer/buffer type shape");
      const auto &element = type(t.type_args[0]);
      if (t.kind == TypeKind::CForeignBuffer)
        need(element.kind == TypeKind::SignedInt ||
                 element.kind == TypeKind::UnsignedInt ||
                 element.kind == TypeKind::Isize ||
                 element.kind == TypeKind::Usize ||
                 element.kind == TypeKind::Float ||
                 element.kind == TypeKind::Bool ||
                 element.kind == TypeKind::TypeParameter,
             "C buffer element type");
      if (element.kind == TypeKind::CForeignOpaque) {
        need(element.c_type && element.c_type <=
                                    m.resolved->project->c_types.size() &&
                 m.resolved->project->c_type(element.c_type).kind ==
                     modules::CDeclKind::opaque,
             "C opaque pointee");
      } else {
        valid_type(t.type_args[0], allowed, depth + 1);
      }
    }
    need(t.kind != TypeKind::CForeignOpaque,
         "C opaque type must be used behind a pointer");
    if (t.kind == TypeKind::CForeignRecord ||
        t.kind == TypeKind::CForeignResource ||
        t.kind == TypeKind::CForeignCallback ||
        t.kind == TypeKind::CForeignOpaque) {
      need(t.c_type && t.c_type <= m.resolved->project->c_types.size() &&
               t.type_args.empty() && !t.bits && !t.declaration.value &&
               !t.binding.value,
           "C named type index");
      const auto &d = m.resolved->project->c_type(t.c_type);
      need((t.kind == TypeKind::CForeignRecord && d.kind == modules::CDeclKind::record) ||
               (t.kind == TypeKind::CForeignResource && d.kind == modules::CDeclKind::resource) ||
               (t.kind == TypeKind::CForeignCallback && d.kind == modules::CDeclKind::callback),
           "C named type declaration kind");
    }
    if (t.kind == TypeKind::TypeParameter) {
      need(allowed.count(t.declaration.value),
           "unbound type parameter in function/declaration");
      need(symbol(t.declaration).kind == modules::SymbolKind::type_parameter,
           "type parameter identity");
    }
    if (t.kind == TypeKind::NominalStruct || t.kind == TypeKind::NominalEnum) {
      auto it = nominals.find(t.declaration.value);
      need(it != nominals.end() && it->second->kind == t.kind,
           "nominal declaration identity/kind");
      need(t.type_args.size() == it->second->params.size(),
           "nominal type argument count");
      for (auto a : t.type_args)
        valid_type(a, allowed, depth + 1);
    }
    if (t.kind == TypeKind::Vec || t.kind == TypeKind::Slice ||
        t.kind == TypeKind::Weak) {
      need(t.type_args.size() == 1,
           t.kind == TypeKind::Vec ? "Vec element arity"
           : t.kind == TypeKind::Slice ? "Slice element arity"
                                       : "Weak target arity");
      valid_type(t.type_args[0], allowed, depth + 1);
      if (t.kind == TypeKind::Weak &&
          !type_contains_parameter(m, t.type_args[0]))
        need(weak_target(m, t.type_args[0]),
             "Weak target is not a canonical source declaration");
    }
    if (t.kind == TypeKind::Task) {
      need(t.type_args.size() == 2, "Task result/error arity");
      // The frame stores status plus optional result and error; a Task value
      // itself never carries a nested callable or inout slot.
      for (auto a : t.type_args)
        valid_type(a, allowed, depth + 1);
    }
    if (t.kind == TypeKind::TraitObject) {
      const TraitDecl *tr = trait_declaration(t.declaration);
      need(tr != nullptr, "dyn type has no real trait declaration");
      need(t.type_args.size() == tr->params.size(),
           "dyn trait application arity");
      need(!t.bits && !t.binding.value && !t.c_type &&
               t.parameters.empty() && !t.result.value && !t.throws_type,
           "dyn type shape");
      for (auto a : t.type_args)
        valid_type(a, allowed, depth + 1);
      check_object_safe(t.declaration, SourceSpan{SourceSnapshotId(0), 0, 0});
    }
    if (t.kind == TypeKind::Function) {
      if (t.throws_type)
        valid_type(*t.throws_type, allowed, depth + 1);
      need(type(t.result).kind != TypeKind::ForeignCallable &&
               (!t.throws_type || type(*t.throws_type).kind != TypeKind::ForeignCallable),
           "internal foreign callable in function output");
      for (auto p : t.parameters) {
        need(type(p.type).kind != TypeKind::ForeignCallable &&
                 (type(p.type).kind != TypeKind::ForeignOpaque ||
                  p.mode == ParamMode::ordinary || p.mode == ParamMode::move),
             "internal callable/external inout parameter");
        need(p.mode == ParamMode::ordinary || p.mode == ParamMode::move ||
                 (p.mode == ParamMode::inout &&
                  inout_slot_type(m, p.type, allowed)),
             "unsupported move/nontrivial inout");
        valid_type(p.type, allowed, depth + 1);
      }
      valid_type(t.result, allowed, depth + 1);
    }
    validated_types.insert({id.value, allowed});
  }
  std::set<uint64_t> allowed(SymbolId f) const {
    std::set<uint64_t> result;
    auto it = templates.find(f.value);
    if (it != templates.end())
      for (auto p : it->second->params)
        result.insert(p.symbol.value);
    return result;
  }
  TypeId instantiated_signature(SymbolId target,
                                const std::vector<TypeId> &args) const {
    const auto &f = function(target);
    auto it = templates.find(target.value);
    if (it == templates.end()) {
      need(args.empty(), "type arguments on concrete function");
      return f.signature;
    }
    need(args.size() == it->second->params.size(),
         "template call argument count");
    constraints(it->second->params, args);
    Substitutions sub;
    for (size_t i = 0; i < args.size(); ++i)
      sub.push_back({it->second->params[i].symbol, args[i]});
    return substitute(f.signature, sub, substituted);
  }
  const NominalDecl &nominal_type(TypeId t) const {
    auto it = nominals.find(type(t).declaration.value);
    need(it != nominals.end(), "missing nominal declaration");
    return *it->second;
  }
  TypeId field_type(TypeId t, const FieldDecl &f) const {
    const auto &d = nominal_type(t);
    Substitutions sub;
    need(d.params.size() == type(t).type_args.size(),
         "nominal substitution arity");
    for (size_t i = 0; i < d.params.size(); ++i)
      sub.push_back({d.params[i].symbol, type(t).type_args[i]});
    return substitute(f.type, sub, substituted);
  }
  void operator_types(BinaryOp op, TypeId a, TypeId b, TypeId result) const {
    auto k = type(a).kind;
    if (k == TypeKind::String) {
      need(a == b && (op == BinaryOp::add ||
                     (op >= BinaryOp::equal && op <= BinaryOp::greater_equal)) &&
               (op == BinaryOp::add ? result == a : type(result).kind == TypeKind::Bool),
           "string operator types");
      return;
    }
    switch (op) {
    case BinaryOp::shift_left:
    case BinaryOp::shift_right:
      need(integer(k) && integer(type(b).kind) && result == a, "shift types");
      break;
    case BinaryOp::add:
    case BinaryOp::subtract:
    case BinaryOp::multiply:
    case BinaryOp::divide:
      need(number(k) && a == b && result == a, "arithmetic types");
      break;
    case BinaryOp::remainder:
    case BinaryOp::bit_and:
    case BinaryOp::bit_or:
    case BinaryOp::bit_xor:
      need(integer(k) && a == b && result == a, "integer operation types");
      break;
    case BinaryOp::equal:
    case BinaryOp::not_equal:
      need((number(k) || k == TypeKind::Bool) && a == b &&
               type(result).kind == TypeKind::Bool,
           "equality types");
      break;
    case BinaryOp::less:
    case BinaryOp::less_equal:
    case BinaryOp::greater:
    case BinaryOp::greater_equal:
      need(number(k) && a == b && type(result).kind == TypeKind::Bool,
           "comparison types");
      break;
    default:
      fail("unknown binary operator");
    }
  }
  TypeId iteration_element(TypeId collection) const {
    const auto &t = type(collection);
    if (t.kind == TypeKind::Vec || t.kind == TypeKind::Slice)
      return t.type_args.at(0);
    need(t.kind == TypeKind::String, "iteration collection type");
    Type rune;
    rune.kind = TypeKind::UnsignedInt;
    rune.bits = 32;
    return substituted.intern(rune);
  }
  void integer_literal(const HirNode &n, const IntegerLiteral &l) const {
    const auto &t = type(n.type);
    need(integer(t.kind), "integer literal type");
    const auto &s = l.decimal;
    need(!s.empty(), "empty integer mathematical value");
    bool neg = s[0] == '-';
    size_t begin = neg ? 1 : 0;
    need(begin < s.size() &&
             !(s[begin] == '0' && (neg || s.size() - begin != 1)),
         "noncanonical decimal integer");
    uint64_t magnitude = 0;
    for (size_t i = begin; i < s.size(); ++i) {
      check_cancelled(stop);
      need(s[i] >= '0' && s[i] <= '9', "nondecimal integer value");
      unsigned d = unsigned(s[i] - '0');
      need(magnitude <= (std::numeric_limits<uint64_t>::max() - d) / 10,
           "integer literal exceeds u64 magnitude");
      magnitude = magnitude * 10 + d;
    }
    unsigned w = bit_width(t, m.config);
    uint64_t mask = w == 64 ? ~uint64_t{0} : (uint64_t{1} << w) - 1;
    uint64_t limit = signed_integer(t.kind) ? (uint64_t{1} << (w - 1)) : mask;
    need(!neg || signed_integer(t.kind), "negative unsigned literal");
    need(magnitude <= (signed_integer(t.kind) && !neg ? limit - 1 : limit),
         "integer literal range");
    need(l.bits == ((neg ? uint64_t{0} - magnitude : magnitude) & mask),
         "integer bit pattern mismatch");
  }
  modules::NodeRef unwrapped(modules::NodeRef ref) const {
    const auto &file = *m.resolved->project->module(ref.module).files.at(ref.file);
    while (file.node(ref.node).kind == AstKind::Parenthesized ||
           file.node(ref.node).kind == AstKind::BracketItem ||
           (file.node(ref.node).kind == AstKind::Unary &&
            file.tokens.at(file.node(ref.node).token_begin).spelling == "+"))
      ref.node = file.node(ref.node).children.at(0);
    return ref;
  }
  std::optional<SymbolId> resolved_source(modules::NodeRef ref) const {
    ref = unwrapped(ref);
    for (const auto &o : m.resolved->occurrences)
      if (same_ref(o.source, ref)) return o.symbol;
    return {};
  }
  const std::string &source_string(modules::NodeRef ref) const {
    std::set<std::tuple<uint64_t, size_t, uint64_t>> visited;
    for (;;) {
      check_cancelled(stop);
      need(visited.emplace(ref.module.value, ref.file, ref.node.value).second,
           "string constant source cycle");
      const auto &file =
          *m.resolved->project->module(ref.module).files.at(ref.file);
      const auto &ast = file.node(ref.node);
      if (ast.kind == AstKind::Parenthesized ||
          ast.kind == AstKind::BracketItem) {
        need(ast.children.size() == 1, "string source wrapper");
        ref.node = ast.children.front();
        continue;
      }
      if (ast.kind == AstKind::Literal) {
        const auto &token = file.tokens.at(ast.token_begin);
        need(token.kind == TokenKind::string_literal && token.string_value,
             "string literal source is not a decoded string");
        return *token.string_value;
      }
      need(ast.kind == AstKind::Name || ast.kind == AstKind::Member,
           "string literal source is not a literal or constant");
      // A qualified member records its alias and declared member on the same
      // AST node; the final occurrence is the actual constant declaration.
      std::optional<SymbolId> target;
      for (const auto &o : m.resolved->occurrences)
        if (same_ref(o.source, ref))
          target = o.symbol;
      need(target && symbol(*target).kind == modules::SymbolKind::constant &&
               symbol(*target).definition,
           "string literal replaced a runtime value");
      ref = *symbol(*target).definition;
      const auto &declaration_file =
          *m.resolved->project->module(ref.module).files.at(ref.file);
      const auto &declaration = declaration_file.node(ref.node);
      size_t token = declaration.token_begin;
      if (declaration_file.tokens.at(token).spelling == "pub")
        ++token;
      need(declaration.kind == AstKind::Binding &&
               declaration_file.tokens.at(token).spelling == "const" &&
               !declaration.children.empty(),
           "string constant source is not a const declaration");
      ref.node = declaration.children.back();
    }
  }
  const TraitDecl *trait_declaration(SymbolId id) const {
    for (const auto &t : m.traits)
      if (t.symbol == id)
        return &t;
    return nullptr;
  }
  bool mentions_self(TypeId id, SymbolId self) const {
    std::vector<TypeId> pending{id};
    std::set<uint64_t> seen;
    while (!pending.empty()) {
      auto current = pending.back();
      pending.pop_back();
      if (!seen.insert(current.value).second)
        continue;
      const auto &t = type(current);
      if (t.kind == TypeKind::TypeParameter && t.declaration == self)
        return true;
      pending.insert(pending.end(), t.type_args.begin(), t.type_args.end());
      for (const auto &p : t.parameters)
        pending.push_back(p.type);
      if (t.kind == TypeKind::Function) {
        pending.push_back(t.result);
        if (t.throws_type)
          pending.push_back(*t.throws_type);
      }
    }
    return false;
  }
  void check_object_safe(SymbolId trait_symbol, SourceSpan span) const {
    auto require = [&](bool ok, std::string_view message) {
      if (!ok)
        fail(std::string(message), span);
    };
    const TraitDecl *tr = trait_declaration(trait_symbol);
    require(tr != nullptr, "dyn trait declaration is missing");
    for (const auto &proto : tr->methods) {
      require(source_parameters(proto.source).empty(),
              "dyn trait method has generic type parameters");
      const auto &sig = type(proto.signature);
      require(!sig.parameters.empty() &&
                  sig.parameters.front().mode == ParamMode::ordinary &&
                  type(sig.parameters.front().type).kind ==
                      TypeKind::TypeParameter &&
                  type(sig.parameters.front().type).declaration == tr->self,
              "dyn trait method has no ordinary Self receiver");
      require(!sig.throws_type, "dyn trait method throws");
      require(!mentions_self(sig.result, tr->self),
              "dyn trait method returns Self");
      for (size_t i = 1; i < sig.parameters.size(); ++i) {
        require(sig.parameters[i].mode == ParamMode::ordinary,
                "dyn trait method has a move/inout parameter");
        require(!mentions_self(sig.parameters[i].type, tr->self),
                "dyn trait method uses Self in a data parameter");
      }
    }
  }

  std::optional<std::pair<const TraitDecl *, const MethodPrototype *>>
  trait_prototype(SymbolId method) const {
    for (const auto &t : m.traits)
      for (const auto &p : t.methods)
        if (p.symbol == method)
          return std::make_pair(&t, &p);
    return {};
  }
  // Rebuild one complete application from the real Source header: the actual
  // trait declaration followed by the written type arguments in order. This
  // never trusts a producer-provided TraitRef or a signature cache.
  TraitRef source_trait_application(modules::NodeRef ref) const {
    // A qualified application such as lib.Carry records the module alias and
    // then the real trait on the same syntax node. The last occurrence is the
    // declared member, matching source_type's qualified-name handling.
    auto target = unwrapped(ref);
    std::optional<SymbolId> declaration;
    for (const auto &o : m.resolved->occurrences)
      if (same_ref(o.source, target))
        declaration = o.symbol;
    need(declaration.has_value() &&
             symbol(*declaration).kind == modules::SymbolKind::trait,
         "trait application has no resolved trait declaration");
    const auto &file =
        *m.resolved->project->module(ref.module).files.at(ref.file);
    const auto &syntax = file.node(ref.node);
    need(syntax.kind == AstKind::NamedType ||
             syntax.kind == AstKind::NeutralBracket ||
             syntax.children.empty(),
         "trait application source kind");
    std::vector<TypeId> args;
    if (syntax.kind == AstKind::NeutralBracket) {
      for (size_t i = 1; i < syntax.children.size(); ++i) {
        auto child = ref;
        child.node = syntax.children[i];
        args.push_back(source_type(child));
      }
    } else {
      for (auto id : syntax.children) {
        auto child = ref;
        child.node = id;
        args.push_back(source_type(child));
      }
    }
    return {*declaration, std::move(args)};
  }
  TypeId parameter_type(SymbolId s) const {
    Type t;
    t.kind = TypeKind::TypeParameter;
    t.declaration = s;
    return substituted.intern(std::move(t));
  }
  TraitRef trait_self_application(const TraitDecl &tr) const {
    std::vector<TypeId> args;
    for (const auto &p : tr.params)
      args.push_back(parameter_type(p.symbol));
    return {tr.symbol, std::move(args)};
  }
  void check_trait_application(const TraitRef &r,
                               const std::set<uint64_t> &allowed) const {
    need(symbol(r.declaration).kind == modules::SymbolKind::trait,
         "trait application declaration is not a trait");
    const auto *tr = trait_declaration(r.declaration);
    need(tr && tr->params.size() == r.args.size(),
         "trait application has the wrong argument count");
    for (auto a : r.args)
      valid_type(a, allowed);
  }
  // Check one complete trait application's method graphs after substituting
  // the real Self and every ordered trait argument. Unresolved parameters
  // from the surrounding template remain deferred, but all concrete callback
  // and top-level inout data uses the same inout_slot_type rule as calls.
  // Substitution interns local TypeIds, so the data helper is given a scratch
  // module whose context is this verifier's substituted context, never the
  // producer's original m.types.
  void check_trait_application_data(const TraitRef &r,
                                    TypeId application) const {
    const auto *tr = trait_declaration(r.declaration);
    if (!tr || tr->params.size() != r.args.size())
      return;
    Substitutions sub{{tr->self, application}};
    for (size_t i = 0; i < tr->params.size(); ++i)
      sub.push_back({tr->params[i].symbol, r.args[i]});
    std::set<uint64_t> base;
    std::set<uint64_t> seen;
    std::function<void(TypeId)> allow = [&](TypeId id) {
      if (!seen.insert(id.value).second)
        return;
      const Type *found = substituted.find(id);
      if (!found)
        found = m.types.find(id);
      need(found != nullptr, "trait application data type is missing");
      const Type t = *found;
      if (t.kind == TypeKind::TypeParameter) {
        base.insert(t.declaration.value);
        return;
      }
      for (auto a : t.type_args)
        allow(a);
      for (const auto &p : t.parameters)
        allow(p.type);
      if (t.kind == TypeKind::Function) {
        allow(t.result);
        if (t.throws_type)
          allow(*t.throws_type);
      }
    };
    allow(application);
    for (auto arg : r.args)
      allow(arg);
    for (const auto &method : tr->methods) {
      std::set<uint64_t> allowed = base;
      for (const auto &p : source_parameters(method.source))
        allowed.insert(p.symbol.value);
      const TypeId sig = substitute(method.signature, sub, substituted);
      std::set<uint64_t> visited, expanded;
      // Prune declarations that cannot reach any inout function. This keeps
      // a growing but purely ordinary function-return graph finite without
      // skipping an application that can actually contain illegal inout.
      std::map<uint64_t, bool> inout_memo;
      auto has_inout = [&](TypeId root) -> bool {
        if (auto it = inout_memo.find(root.value); it != inout_memo.end())
          return it->second;
        // Cache only a completed reachability search, never a cycle back-edge.
        std::set<uint64_t> seen_types, seen_declarations;
        std::vector<TypeId> pending{root};
        while (!pending.empty()) {
          const auto id = pending.back();
          pending.pop_back();
          if (!seen_types.insert(id.value).second)
            continue;
          const Type *found = substituted.find(id);
          need(found != nullptr, "trait application inout type is missing");
          const Type t = *found;
          pending.insert(pending.end(), t.type_args.begin(), t.type_args.end());
          if (t.kind == TypeKind::Function) {
            for (const auto &p : t.parameters) {
              if (p.mode == ParamMode::inout)
                return inout_memo[root.value] = true;
              pending.push_back(p.type);
            }
            pending.push_back(t.result);
            if (t.throws_type)
              pending.push_back(*t.throws_type);
          }
          if (t.kind != TypeKind::NominalStruct &&
              t.kind != TypeKind::NominalEnum)
            continue;
          const NominalDecl *decl = nullptr;
          for (const auto &d : m.nominals)
            if (d.symbol == t.declaration) {
              decl = &d;
              break;
            }
          need(decl != nullptr, "trait application nominal is missing");
          if (!seen_declarations.insert(decl->symbol.value).second)
            continue;
          for (const auto &f : decl->fields)
            pending.push_back(f.type);
          for (const auto &v : decl->variants)
            for (const auto &f : v.payload)
              pending.push_back(f.type);
        }
        return inout_memo[root.value] = false;
      };
      // Visit each depth before expanding a growing return graph further.
      std::vector<std::pair<TypeId, size_t>> work{{sig, 0}};
      auto visit = [&](TypeId id, size_t depth) { work.emplace_back(id, depth); };
      for (size_t next = 0; next < work.size(); ++next) {
        const auto [id, depth] = work[next];
        need(depth < 128, "trait application inout graph exceeds budget");
        const Type *found = substituted.find(id);
        need(found != nullptr, "trait application inout type is missing");
        const Type t = *found;
        if (t.kind == TypeKind::TypeParameter)
          continue;
        if (t.kind == TypeKind::Vec || t.kind == TypeKind::Slice) {
          if (visited.insert(id.value).second)
            for (auto a : t.type_args)
              visit(a, depth + 1);
          continue;
        }
        if (t.kind == TypeKind::NominalStruct ||
            t.kind == TypeKind::NominalEnum) {
          if (visited.insert(id.value).second)
            for (auto a : t.type_args)
              visit(a, depth + 1);
          const NominalDecl *decl = nullptr;
          for (const auto &d : m.nominals)
            if (d.symbol == t.declaration) {
              decl = &d;
              break;
            }
          need(decl != nullptr, "trait application nominal is missing");
          need(decl->params.size() == t.type_args.size(),
               "trait application nominal argument count");
          // Distinct complete applications must each have their fields checked.
          if (expanded.insert(id.value).second && has_inout(id)) {
            Substitutions fields;
            for (size_t i = 0; i < decl->params.size(); ++i)
              fields.push_back(
                  {decl->params[i].symbol, t.type_args[i]});
            auto field = [&](const FieldDecl &f) {
              visit(substitute(f.type, fields, substituted), depth + 1);
            };
            for (const auto &f : decl->fields)
              field(f);
            for (const auto &v : decl->variants)
              for (const auto &f : v.payload)
                field(f);
          }
          continue;
        }
        if (t.kind != TypeKind::Function ||
            !visited.insert(id.value).second)
          continue;
        for (const auto &p : t.parameters) {
          if (p.mode == ParamMode::inout) {
            HirModule context;
            context.types = substituted;
            context.nominals = m.nominals;
            need(inout_slot_type(context, p.type, allowed),
                 "trait application inout data is not a whole Woozy value");
          }
          visit(p.type, depth + 1);
        }
        visit(t.result, depth + 1);
        if (t.throws_type)
          visit(*t.throws_type, depth + 1);
      }
    }
  }
  // Independently apply a trait's real ordered parameter bounds. The
  // substitution is exactly [trait.params -> args, trait.Self -> application
  // Self], and each resulting bound is checked against its argument. Nested
  // applications are visited before the one-impl relation, so a partly built
  // producer cache cannot hide a missing or reordered constraint.
  void check_trait_parameter_bounds(const TraitRef &r, TypeId application,
                                    size_t depth = 0) const {
    const auto *tr = trait_declaration(r.declaration);
    need(tr != nullptr, "trait application has no real declaration");
    need(tr->params.size() == r.args.size(),
         "trait application has the wrong argument count");
    need(depth < 64, "trait constraint graph exceeds verification budget");
    std::vector<uint64_t> args;
    for (auto a : r.args)
      args.push_back(a.value);
    auto key = std::make_tuple(application.value, r.declaration.value,
                               std::move(args));
    if (!active_trait_bounds.insert(key).second)
      return;
    Substitutions sub{{tr->self, application}};
    for (size_t i = 0; i < tr->params.size(); ++i)
      sub.push_back({tr->params[i].symbol, r.args[i]});
    for (size_t i = 0; i < tr->params.size(); ++i) {
      if (!tr->params[i].trait)
        continue;
      TraitRef bound = *tr->params[i].trait;
      for (auto &arg : bound.args)
        arg = substitute(arg, sub, substituted);
      check_trait_parameter_bounds(bound, r.args[i], depth + 1);
      need(satisfies(r.args[i], bound),
           "trait argument does not satisfy the trait declaration's "
           "parameter constraint");
    }
    check_trait_application_data(r, application);
    active_trait_bounds.erase(key);
  }
  struct SourceMethodCall {
    modules::NodeRef base;
    std::optional<SymbolId> base_symbol;
    std::string name;
    bool explicit_type_arguments = false;
    std::vector<modules::NodeRef> explicit_arguments;
  };
  // Rebuild the written call form from the owning Source. A Member with its
  // own resolved occurrence is a module/qualified-name call, not a method.
  // Transparent parentheses and the optional explicit method type suffix do
  // not change the base. The base's real occurrence decides value vs type.
  // Rebuild a method member expression, with its optional method-own type
  // suffix, independent of whether the surrounding Source is a Call or a
  // function-value expression. Transparent parentheses and the suffix do not
  // change the base; the base's real occurrence decides value vs type.
  std::optional<SourceMethodCall>
  source_method_member(modules::NodeRef ref) const {
    const auto &file =
        *m.resolved->project->module(ref.module).files.at(ref.file);
    ref = unwrapped(ref);
    bool explicit_type_arguments = false;
    std::vector<modules::NodeRef> explicit_arguments;
    if (file.node(ref.node).kind == AstKind::NeutralBracket) {
      need(!file.node(ref.node).children.empty(),
           "method type suffix has no target");
      explicit_type_arguments = true;
      for (size_t i = 1; i < file.node(ref.node).children.size(); ++i) {
        auto argument = ref;
        argument.node = file.node(ref.node).children[i];
        explicit_arguments.push_back(argument);
      }
      ref.node = file.node(ref.node).children.front();
      ref = unwrapped(ref);
    }
    const auto &member = file.node(ref.node);
    if (member.kind != AstKind::Member || resolved_source(ref))
      return {};
    need(!member.children.empty(), "method member has no base");
    auto base = ref;
    base.node = member.children.front();
    auto symbolized = base;
    for (;;) {
      symbolized = unwrapped(symbolized);
      const auto &a = file.node(symbolized.node);
      if (a.kind != AstKind::NeutralBracket)
        break;
      need(!a.children.empty(), "type qualifier has no base");
      symbolized.node = a.children.front();
    }
    auto base_symbol = resolved_source(symbolized);
    need(member.token_end > member.token_begin,
         "method member token range");
    return SourceMethodCall{base, base_symbol,
                            std::string(file.tokens[member.token_end - 1]
                                            .spelling),
                            explicit_type_arguments, explicit_arguments};
  }
  std::optional<SourceMethodCall> source_method_call(const HirNode &n) const {
    const auto &file =
        *m.resolved->project->module(n.source.node.module)
             .files.at(n.source.node.file);
    auto ref = n.source.node;
    const auto &call = file.node(ref.node);
    if (call.kind != AstKind::Call || call.children.empty())
      return {};
    ref.node = call.children.front();
    return source_method_member(ref);
  }
  std::pair<modules::NodeRef, ParamMode>
  receiver_input(modules::NodeRef base) const {
    const auto &file =
        *m.resolved->project->module(base.module).files.at(base.file);
    auto parentheses = [&](modules::NodeRef ref) {
      while (file.node(ref.node).kind == AstKind::Parenthesized)
        ref.node = file.node(ref.node).children.at(0);
      return ref;
    };
    base = parentheses(base);
    const auto &syntax = file.node(base.node);
    auto mode = ParamMode::ordinary;
    if (syntax.kind == AstKind::Unary) {
      auto marker = file.tokens.at(syntax.token_begin).spelling;
      if (marker == "move")
        mode = ParamMode::move;
      if (marker == "inout") {
        mode = ParamMode::inout;
        need(syntax.children.size() == 1, "inout receiver has no operand");
        base.node = syntax.children.front();
        base = parentheses(base);
      }
    }
    // Unary + is a value operation, not an inout place or a receiver marker.
    return {mode == ParamMode::ordinary ? unwrapped(base) : base, mode};
  }
  void check_receiver_source(const HirNode &n, const std::vector<CallArg> &args,
                             modules::NodeRef base, ParamMode declared) const {
    auto [input, mode] = receiver_input(base);
    const auto &file =
        *m.resolved->project->module(base.module).files.at(base.file);
    need(!args.empty() &&
             args.size() == file.node(n.source.node.node).children.size(),
         "value call receiver differs from source arguments (source arity)");
    const auto &receiver = args.front();
    const auto &value = node(receiver.expression);
    need(receiver.source_index == 0 && receiver.parameter_index == 0 &&
             !receiver.label && receiver.mode == mode && mode == declared &&
             same_ref(unwrapped(value.source.node), input),
         "value call receiver source/mode differs from the real Member base");
    if (mode != ParamMode::ordinary) {
      const auto &actual =
          *m.resolved->project->module(value.source.node.module)
               .files.at(value.source.node.file);
      need(same_span(value.source.span,
                     actual.node(value.source.node.node).span),
           "explicit receiver source span differs from its complete AST node");
      need(mode == ParamMode::move ? value.kind() == NodeKind::move
                                   : inout_place(value),
           "explicit receiver must keep its Move or writable Place");
      if (mode == ParamMode::inout && value.kind() == NodeKind::place) {
        auto binding = resolved_source(input);
        need(binding.has_value(), "inout receiver has no source binding");
        for (const auto &instance : m.instances)
          if (instance.symbol == scope(n.scope).function)
            for (auto [from, to] : instance.renamed)
              if (from == *binding) {
                binding = to;
                break;
              }
        need(std::get<Place>(value.data).symbol == *binding,
             "inout receiver Place differs from its real source binding");
      }
    }
  }
  bool inherent_self_source(SymbolId method, const ImplDecl &impl) const {
    const auto &source = function(method).source;
    const auto &file = *m.resolved->project->module(source.node.module)
                            .files.at(source.node.file);
    for (auto id : file.node(source.node.node).children) {
      const auto &parameter = file.node(id);
      if (parameter.kind != AstKind::Parameter)
        continue;
      need(!parameter.children.empty(), "method parameter has no source type");
      auto raw = source.node;
      raw.node = parameter.children.front();
      return resolved_source(raw) == impl.self_parameter;
    }
    return false;
  }
  const ForeignBinding &foreign_binding(ForeignBindingId id) const {
    need(id.value < m.foreign_bindings.size(), "foreign binding index");
    return m.foreign_bindings[id.value];
  }
  void foreign_read(HirNodeId id) const {
    const auto &n = node(id);
    auto binding = std::get<ForeignRead>(n.data).binding;
    const auto &b = foreign_binding(binding);
    const auto &d = m.resolved->project->foreign_binding(binding);
    need(n.type == b.type, "foreign read result type");
    if (b.signature) {
      size_t uses = 0;
      for (const auto &parent : m.nodes)
        for (auto ch : parent.children)
          if (ch == id) {
            auto call = std::get_if<Call>(&parent.data);
            need(call && call->kind == CalleeKind::foreign && call->callee == id &&
                     call->binding == binding, "foreign callable escaped adjacent call");
            ++uses;
          }
      need(uses == 1, "foreign callable must have one adjacent call");
    } else
      need(std::holds_alternative<modules::BindingValue>(d.detail),
           "external type declaration is not a runtime value");
    auto ref = unwrapped(n.source.node);
    const auto &file = *m.resolved->project->module(ref.module).files.at(ref.file);
    const auto &ast = file.node(ref.node);
    if (b.owner) {
      need(n.children.size() == 1 && expr(n.children[0]).type == *b.owner,
           "foreign receiver type/arity");
      const auto &c = std::get<modules::BindingCallable>(d.detail);
      auto receiver_ref = ref;
      if (c.call_kind == "method") {
        need(ast.kind == AstKind::Member && file.tokens.at(ast.token_end - 1).spelling == d.name,
             "foreign source method name");
        receiver_ref.node = ast.children.at(0);
      } else need(c.call_kind == "callable", "foreign receiver call kind");
      need(same_ref(unwrapped(node(n.children[0]).source.node), unwrapped(receiver_ref)),
           "foreign receiver differs from source");
      const auto &owner = m.resolved->project->foreign_binding(type(*b.owner).binding);
      const auto &members = std::get<modules::BindingType>(owner.detail).members;
      need(std::find(members.begin(), members.end(), binding) != members.end(),
           "foreign owner member identity");
    } else {
      need(n.children.empty(), "module foreign read cannot have receiver");
      auto sid = resolved_source(ref);
      need(sid.has_value(), "foreign export source has no symbol");
      auto ex = m.resolved->foreign_export(*sid);
      need(ex && (b.signature ? std::find(ex->candidates.begin(), ex->candidates.end(), binding) != ex->candidates.end()
                             : ex->declaration == binding), "foreign read differs from resolved export");
    }
  }
  void foreign_access(HirNodeId id) const {
    const auto &n = node(id);
    const auto &access = std::get<ForeignRead>(n.data);
    const auto &b = foreign_binding(access.binding);
    const auto &d = m.resolved->project->foreign_binding(access.binding);
    need(n.type == b.type, "foreign access result type");
    need(n.children.size() >= 1, "foreign access receiver arity");
    const auto &receiver = expr(n.children[0]);
    need(type(receiver.type).kind == TypeKind::ForeignOpaque,
         "foreign access receiver type");
    need(type(receiver.type).binding.value < m.foreign_bindings.size(),
         "foreign access receiver binding");
    const auto &owner = m.resolved->project->foreign_binding(
        type(receiver.type).binding);
    const bool place = foreign_access_place(n);
    const bool read = foreign_access_read(n);
    need(read || place, "foreign access kind");
    auto matches_declared = [&](TypeId actual, const std::string &name) {
      TypeContext scratch = m.types;
      auto expected = detail::binding_type(*m.resolved->project, scratch, name);
      const auto *want = scratch.find(expected);
      const auto *have = m.types.find(actual);
      return want && have && *want == *have;
    };
    const auto &file = *m.resolved->project->module(n.source.node.module)
                            .files.at(n.source.node.file);
    const auto &ast = file.node(n.source.node.node);
    auto source_child = [&](HirNodeId child, AstNodeId syntax) {
      auto expected = n.source.node;
      expected.node = syntax;
      return same_ref(unwrapped(node(child).source.node), unwrapped(expected));
    };
    if (auto property = std::get_if<modules::BindingProperty>(&d.detail)) {
      need(!access.start && !access.end, "property access endpoint");
      need(n.children.size() == 1, "property receiver arity");
      need(owner.identity == property->owner_type, "property owner identity");
      need(place ? !property->setter.empty() : !property->getter.empty(),
           "property access permission");
      need(matches_declared(receiver.type, property->owner_type),
           "property receiver declaration type");
      need(ast.kind == AstKind::Member && ast.token_end > ast.token_begin &&
               ast.token_end <= file.tokens.size(),
           "property source is not a real Member");
      need(file.tokens.at(ast.token_end - 1).spelling == property->name,
           "property source name differs from declaration");
      need(source_child(n.children[0], ast.children.at(0)),
           "property receiver differs from source");
      return;
    }
    if (auto index = std::get_if<modules::BindingIndexer>(&d.detail)) {
      need(!access.start && !access.end, "index access endpoint");
      need(n.children.size() == 2, "index access arity");
      need(owner.identity == index->owner_type, "index owner identity");
      need(place ? index->writable : index->readable,
           "index access permission");
      need(matches_declared(receiver.type, index->owner_type),
           "index receiver declaration type");
      need(matches_declared(expr(n.children[1]).type, index->index_type),
           "index argument declaration type");
      need(ast.kind == AstKind::NeutralBracket && ast.children.size() == 2,
           "index source is not a real NeutralBracket");
      need(ast.children.size() == n.children.size(), "index source arity");
      need(source_child(n.children[0], ast.children[0]) &&
               source_child(n.children[1], ast.children[1]),
           "index receiver/key differs from source");
      return;
    }
    auto slice = std::get_if<modules::BindingSlice>(&d.detail);
    need(slice != nullptr, "foreign access declaration category");
    need(owner.identity == slice->owner_type, "slice owner identity");
    need(place ? slice->writable : slice->readable, "slice access permission");
    need(matches_declared(receiver.type, slice->owner_type),
         "slice receiver declaration type");
    need(ast.kind == AstKind::Slice, "slice source is not a real Slice");
    bool start_syntax =
        ast.children.size() > 1 &&
        file.node(ast.children[1]).kind == AstKind::BracketItem;
    bool end_syntax =
        ast.children.size() == 3 ||
        (ast.children.size() == 2 && !start_syntax);
    need(bool(access.start) == start_syntax &&
             bool(access.end) == end_syntax,
         "slice omitted endpoint differs from source");
    need(n.children.size() == 1 + size_t(start_syntax) + size_t(end_syntax),
         "slice child arity");
    size_t next = 1;
    if (access.start) {
      need(n.children[next++] == *access.start, "slice start child");
      if (slice->start_type)
        need(matches_declared(expr(*access.start).type, *slice->start_type),
             "slice start declaration type");
      else
        need(false, "slice start supplied without declaration");
    }
    if (access.end) {
      need(n.children[next++] == *access.end, "slice end child");
      if (slice->end_type)
        need(matches_declared(expr(*access.end).type, *slice->end_type),
             "slice end declaration type");
      else
        need(false, "slice end supplied without declaration");
    }
    need(source_child(n.children[0], ast.children[0]),
         "slice receiver differs from source");
    if (access.start)
      need(source_child(*access.start, ast.children[1]),
           "slice start differs from source");
    if (access.end)
      need(source_child(*access.end, ast.children.back()),
           "slice end differs from source");
  }
  void c_callback_type(TypeId function, uint32_t c_type) const {
    const auto &decl = m.resolved->project->c_type(c_type);
    need(decl.kind == modules::CDeclKind::callback,
         "C callback declaration kind");
    const auto &sig = type(function);
    need(sig.kind == TypeKind::Function && !sig.throws_type &&
             sig.parameters.size() == decl.parameters.size(),
         "C callback source signature");
    TypeContext scratch = m.types;
    for (size_t i = 0; i < decl.parameters.size(); ++i) {
      auto expected = detail::c_type_id(*m.resolved->project, scratch,
                                        decl.parameters[i].type);
      need(sig.parameters[i].mode == ParamMode::ordinary &&
               sig.parameters[i].type == expected,
           "C callback parameter type");
    }
    need(sig.result == detail::c_type_id(*m.resolved->project, scratch,
                                         decl.result),
         "C callback result type");
  }
  void c_foreign_call(const HirNode &n, const Call &c) const {
    need(c.c_callable.has_value() && !c.binding && !c.callee &&
             c.type_args.empty() && c.omitted.empty(),
         "C call metadata");
    const auto &project = *m.resolved->project;
    need(*c.c_callable >= 1 && *c.c_callable <= project.c_callables.size(),
         "C callable index");
    const auto &decl = project.c_callable(*c.c_callable);
    TypeContext scratch = m.types;
    TypeId expected;
    try {
      expected = detail::c_callable_signature(project, scratch, decl);
    } catch (const std::invalid_argument &e) {
      need(false, "C callable signature is invalid");
    }
    need(expected == c.signature, "C call signature differs from frozen binding");
    const auto &sig = type(c.signature);
    need(sig.kind == TypeKind::Function && !sig.throws_type &&
             n.type == sig.result && sig.parameters.size() == c.args.size(),
         "C call signature/result/arity");
    const auto &file = *project.module(n.source.node.module).files.at(n.source.node.file);
    const auto &ast = file.node(n.source.node.node);
    need(ast.kind == AstKind::Call && ast.children.size() == c.args.size() + 1,
         "C call source arity");
    auto callee_ref = n.source.node;
    callee_ref.node = ast.children[0];
    auto sid = resolved_source(unwrapped(callee_ref));
    need(sid.has_value(), "C call source has no export identity");
    auto ex = m.resolved->foreign_export(*sid);
    need(ex && ex->c_callable == *c.c_callable,
         "C call source export differs from binding identity");
    need(n.children.size() == c.args.size(), "C call argument children");
    for (size_t i = 0; i < c.args.size(); ++i) {
      const auto &a = c.args[i];
      need(a.source_index == i && a.parameter_index == i && !a.label &&
               a.mode == ParamMode::ordinary,
           "C argument order/label/mode");
      need(a.expression == n.children[i], "C argument child identity");
      const auto &syntax = file.node(ast.children[i + 1]);
      need(syntax.kind == AstKind::Argument && syntax.children.size() == 1 &&
               file.tokens.at(syntax.token_begin + 1).spelling != ":",
           "C call positional argument source");
      auto ref = n.source.node;
      ref.node = syntax.children[0];
      auto er = unwrapped(ref);
      const auto &e = file.node(er.node);
      need(!(e.kind == AstKind::Unary &&
             (file.tokens.at(e.token_begin).spelling == "move" ||
              file.tokens.at(e.token_begin).spelling == "inout")),
           "C argument mode");
      const auto &argument = expr(a.expression);
      need(same_ref(unwrapped(argument.source.node), er),
           "C argument source identity");
      const auto expected_type = sig.parameters[i].type;
      if (type(expected_type).kind == TypeKind::CForeignCallback) {
        need(argument.kind() == NodeKind::function_value,
             "C callback argument is not an uncaptured function value");
        const auto &callback = std::get<FunctionValue>(argument.data);
        need(callback.captures.empty() && !callback.bound &&
                 !callback.move_capture,
             "C callback cannot capture or bind a receiver");
        c_callback_type(argument.type, type(expected_type).c_type);
      } else {
        bool compatible = argument.type == expected_type;
        if (!compatible) {
          const auto &actual = type(argument.type);
          const auto &wanted = type(expected_type);
          compatible = (wanted.kind == TypeKind::CForeignPointer ||
                        wanted.kind == TypeKind::CForeignBuffer) &&
                       actual.kind == wanted.kind && wanted.bits == 0 &&
                       actual.bits == 1 &&
                       actual.type_args == wanted.type_args;
        }
        need(compatible, "C argument type differs from binding");
      }
    }
  }
  void c_field_read(const HirNode &n) const {
    const auto &d = std::get<CFieldRead>(n.data);
    need(d.c_type >= 1 && d.c_type <= m.resolved->project->c_types.size(),
         "C field record index");
    const auto &decl = m.resolved->project->c_type(d.c_type);
    need(decl.kind == modules::CDeclKind::record && d.field < decl.fields.size(),
         "C field declaration/index");
    need(n.children.size() == 1, "C field child count");
    const auto &base = expr(n.children[0]);
    const auto base_type = type(base.type);
    need(base_type.kind == TypeKind::CForeignRecord &&
             base_type.c_type == d.c_type,
         "C field receiver type");
    TypeContext scratch = m.types;
    auto expected = detail::c_type_id(*m.resolved->project, scratch,
                                      decl.fields[d.field].type);
    need(n.type == expected, "C field result type differs from declaration");
    const auto &file = *m.resolved->project->module(n.source.node.module).files.at(n.source.node.file);
    const auto &ast = file.node(n.source.node.node);
    need(ast.kind == AstKind::Member && ast.children.size() == 1,
         "C field source is not a member");
    auto ref = n.source.node;
    ref.node = ast.children[0];
    need(same_ref(unwrapped(base.source.node), unwrapped(ref)),
         "C field base differs from source receiver");
    const auto name = file.tokens.at(ast.token_end - 1).spelling;
    need(name == decl.fields[d.field].name,
         "C field name differs from declaration");
  }
  void foreign_call(const HirNode &n, const Call &c) const {
    need(c.binding.has_value() && c.callee.has_value() && !c.direct_symbol && c.type_args.empty(),
         "foreign call metadata");
    const auto &b = foreign_binding(*c.binding);
    need(b.signature == c.signature && n.type == type(c.signature).result,
         "foreign call signature/result");
    const auto &target = expr(*c.callee);
    auto read = std::get_if<ForeignRead>(&target.data);
    need(read && read->binding == *c.binding && target.type == b.type,
         "foreign call target identity");
    const auto &decl = std::get<modules::BindingCallable>(m.resolved->project->foreign_binding(*c.binding).detail);
    const auto &file = *m.resolved->project->module(n.source.node.module).files.at(n.source.node.file);
    const auto &ast = file.node(n.source.node.node);
    need(ast.kind == AstKind::Call && ast.children.size() == c.args.size() + 1 &&
             n.children.size() == c.args.size() + 1 && n.children[0] == *c.callee,
         "foreign call source/children arity and target order");
    auto ref = n.source.node; ref.node = ast.children[0];
    need(same_ref(unwrapped(target.source.node), unwrapped(ref)), "foreign call source target");
    std::vector<std::optional<std::string>> labels;
    for (size_t i = 0; i < c.args.size(); ++i) {
      const auto &a = c.args[i];
      const auto &syntax = file.node(ast.children[i + 1]);
      need(syntax.kind == AstKind::Argument && syntax.children.size() == 1,
           "foreign source argument kind");
      std::optional<std::string> label;
      if (file.tokens.at(syntax.token_begin + 1).spelling == ":")
        label = std::string(file.tokens.at(syntax.token_begin).spelling);
      labels.push_back(label);
      ref.node = syntax.children[0];
      auto er = unwrapped(ref);
      const auto &e = file.node(er.node);
      need(!(e.kind == AstKind::Unary &&
             (file.tokens.at(e.token_begin).spelling == "move" ||
              file.tokens.at(e.token_begin).spelling == "inout")), "foreign source argument mode");
      need(a.source_index == i && a.label == label && a.mode == ParamMode::ordinary &&
               n.children[i + 1] == a.expression &&
               same_ref(unwrapped(expr(a.expression).source.node), er),
           "foreign argument source order/identity/label/mode");
    }
    auto bound = detail::bind_arguments(decl, labels);
    need(bound.has_value() && bound->omitted == c.omitted, "foreign missing/optional parameters");
    std::vector<ForeignBindingId> candidates;
    if (b.owner) {
      const auto &owner = m.resolved->project->foreign_binding(type(*b.owner).binding);
      const auto &name = m.resolved->project->foreign_binding(*c.binding).name;
      for (auto member : std::get<modules::BindingType>(owner.detail).members) {
        const auto &d = m.resolved->project->foreign_binding(member);
        const auto &other = std::get<modules::BindingCallable>(d.detail);
        if (other.call_kind == decl.call_kind && d.name == name) candidates.push_back(member);
      }
    } else {
      auto sid = resolved_source(target.source.node);
      need(sid.has_value(), "foreign candidate source identity");
      auto ex = m.resolved->foreign_export(*sid);
      need(ex != nullptr, "foreign candidate source export");
      candidates = ex->candidates;
    }
    size_t matches = 0;
    for (auto candidate : candidates) {
      const auto &other = std::get<modules::BindingCallable>(m.resolved->project->foreign_binding(candidate).detail);
      auto mapping = detail::bind_arguments(other, labels);
      if (!mapping) continue;
      const auto &signature = type(*foreign_binding(candidate).signature);
      bool match = true;
      for (size_t i = 0; i < c.args.size(); ++i)
        match &= expr(c.args[i].expression).type == signature.parameters.at(mapping->parameters[i]).type;
      matches += match;
    }
    need(matches == 1, "foreign candidate is not uniquely selected");
    const auto &sig = type(c.signature);
    for (size_t i = 0; i < c.args.size(); ++i) {
      const auto &a = c.args[i];
      need(a.parameter_index == bound->parameters[i] &&
               sig.parameters.at(a.parameter_index).mode == ParamMode::ordinary &&
               expr(a.expression).type == sig.parameters.at(a.parameter_index).type,
           "foreign argument parameter index/type");
    }
  }
  bool inout_place(const HirNode &n) const {
    return n.kind() == NodeKind::place ||
           n.kind() == NodeKind::field_place ||
           n.kind() == NodeKind::index_place;
  }
  // Root binding and field/index path of a place. A path segment is either a
  // named field or an index expression. Index segments are treated as an
  // unknown offset, so two different dynamic indexes into one container are
  // conservatively overlapping; distinct named fields are disjoint.
  struct PathSegment {
    bool index = false;
    bool known = true;
    // SymbolId for a field; a literal value or HirNodeId for an index.
    uint64_t value = 0;
  };
  std::optional<SymbolId> place_root(HirNodeId id) const {
    const auto &n = node(id);
    if (auto r = std::get_if<Read>(&n.data))
      return r->symbol;
    if (auto p = std::get_if<Place>(&n.data))
      return p->symbol;
    if (n.kind() == NodeKind::field_place || n.kind() == NodeKind::field_read ||
        n.kind() == NodeKind::index_place || n.kind() == NodeKind::index_read)
      return place_root(n.children[0]);
    return {};
  }
  std::vector<PathSegment> place_path(HirNodeId id) const {
    const auto &n = node(id);
    if (n.kind() == NodeKind::field_place || n.kind() == NodeKind::field_read) {
      auto path = place_path(n.children[0]);
      auto field = n.kind() == NodeKind::field_place
                       ? std::get<FieldPlace>(n.data).field
                       : std::get<FieldRead>(n.data).field;
      path.push_back({false, true, field.value});
      return path;
    }
    if (n.kind() == NodeKind::index_place || n.kind() == NodeKind::index_read) {
      auto path = place_path(n.children[0]);
      const auto &index = m.nodes.at(n.children[1].value);
      const bool known = index.kind() == NodeKind::integer_literal;
      path.push_back({true, known,
                      known ? std::get<IntegerLiteral>(index.data).bits
                            : n.children[1].value});
      return path;
    }
    return {};
  }
  static bool paths_overlap(const std::vector<PathSegment> &a,
                            const std::vector<PathSegment> &b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
      if (a[i].index || b[i].index) {
        if (!a[i].index || !b[i].index)
          return true; // field vs element of the same container
        if (a[i].known && b[i].known && a[i].value == b[i].value)
          continue;
        if (a[i].known && b[i].known)
          return false; // distinct literal indexes are disjoint
        return true;    // unknown dynamic index may alias
      }
      if (a[i].value != b[i].value)
        return false;
    }
    return true;
  }
  using PlaceAccess = std::pair<SymbolId, std::vector<PathSegment>>;
  // A Vec/Slice handle can be copied through bindings, fields and enum
  // payloads. Field selections before an index choose a handle and prove no
  // object identity; index literals plus the field suffix after the last index
  // identify storage within the referenced object.
  struct ContainerPath {
    std::vector<PathSegment> indexes;
    std::vector<uint64_t> fields;
  };
  static ContainerPath container_path(const std::vector<PathSegment> &path) {
    ContainerPath result;
    for (const auto &part : path) {
      if (part.index) {
        result.indexes.push_back(part);
        result.fields.clear();
      } else if (!result.indexes.empty()) {
        result.fields.push_back(part.value);
      }
    }
    return result;
  }
  static bool container_overlap(const ContainerPath &a,
                                const ContainerPath &b) {
    if (a.indexes.size() != b.indexes.size())
      return true;
    // Ancestor elements may contain copies of the same inner Vec handle.
    // Only the final element offset can prove that two places are disjoint.
    const auto &left = a.indexes.back();
    const auto &right = b.indexes.back();
    if (!left.known || !right.known)
      return true;
    if (left.value != right.value)
      return false;
    size_t n = std::min(a.fields.size(), b.fields.size());
    for (size_t i = 0; i < n; ++i)
      if (a.fields[i] != b.fields[i])
        return false;
    return true;
  }
  static bool places_overlap(SymbolId root_a,
                             const std::vector<PathSegment> &a, SymbolId root_b,
                             const std::vector<PathSegment> &b) {
    auto ca = container_path(a), cb = container_path(b);
    if (!ca.indexes.empty() && !cb.indexes.empty())
      return container_overlap(ca, cb);
    return root_a == root_b && paths_overlap(a, b);
  }
  bool may_hold_shared(TypeId root) const {
    TypeContext types = m.types;
    std::set<uint64_t> active;
    std::function<bool(TypeId)> visit = [&](TypeId id) -> bool {
      const Type *found = types.find(id);
      if (!found)
        return true;
      const Type t = *found;
      if (t.kind == TypeKind::Vec || t.kind == TypeKind::Slice ||
          t.kind == TypeKind::Weak || t.kind == TypeKind::TraitObject ||
          t.kind == TypeKind::Function ||
          t.kind == TypeKind::TypeParameter || t.kind == TypeKind::Task)
        return true;
      if (t.kind != TypeKind::NominalStruct &&
          t.kind != TypeKind::NominalEnum)
        return false;
      const NominalDecl *decl = nullptr;
      for (const auto &d : m.nominals)
        if (d.symbol == t.declaration) {
          decl = &d;
          break;
        }
      if (!decl || t.type_args.size() != decl->params.size())
        return true;
      if (!active.insert(id.value).second)
        return false;
      Substitutions sub;
      for (size_t i = 0; i < decl->params.size(); ++i)
        sub.push_back({decl->params[i].symbol, t.type_args[i]});
      auto fields = [&](const auto &fs) {
        for (const auto &field : fs) {
          auto concrete = substitute(field.type, sub, types);
          if (visit(concrete))
            return true;
        }
        return false;
      };
      bool result = fields(decl->fields);
      for (const auto &variant : decl->variants)
        result = result || fields(variant.payload);
      active.erase(id.value);
      return result;
    };
    return visit(root);
  }
  // Rooted field/index receivers are not rescanned as whole containers; their
  // index expressions are scanned instead. This keeps `items[1]` a disjoint
  // scalar read while `rows[prepare(items)].value` still exposes `items`.
  void collect_alias_risks(HirNodeId id, std::vector<TypeId> &out) const {
    const auto &n = node(id);
    const bool storage =
        n.kind() == NodeKind::read || n.kind() == NodeKind::place ||
        n.kind() == NodeKind::field_read ||
        n.kind() == NodeKind::field_place ||
        n.kind() == NodeKind::index_read ||
        n.kind() == NodeKind::index_place;
    if ((storage || n.kind() == NodeKind::function_value ||
         (n.kind() == NodeKind::move &&
          std::get<Move>(n.data).binding)) &&
        may_hold_shared(n.type))
      out.push_back(n.type);
    if (n.kind() == NodeKind::read || n.kind() == NodeKind::place ||
        n.kind() == NodeKind::function_value ||
        (n.kind() == NodeKind::move && std::get<Move>(n.data).binding))
      return;
    if (n.kind() == NodeKind::field_read ||
        n.kind() == NodeKind::field_place ||
        n.kind() == NodeKind::index_read ||
        n.kind() == NodeKind::index_place) {
      if (place_root(id)) {
        collect_place_index_risks(id, out);
        return;
      }
    }
    for (auto c : n.children)
      collect_alias_risks(c, out);
  }
  void collect_place_index_risks(HirNodeId id,
                                 std::vector<TypeId> &out) const {
    const auto &n = node(id);
    if (n.kind() == NodeKind::field_read ||
        n.kind() == NodeKind::field_place) {
      collect_place_index_risks(n.children[0], out);
      return;
    }
    if (n.kind() == NodeKind::index_read ||
        n.kind() == NodeKind::index_place) {
      collect_place_index_risks(n.children[0], out);
      collect_alias_risks(n.children[1], out);
    }
  }
  // Collects the maximal storage paths read by an expression. A field/index
  // receiver is represented by its complete path, while every index
  // expression in that receiver chain is scanned separately because it can
  // read unrelated bindings.
  void collect_access_paths(HirNodeId id,
                            std::vector<PlaceAccess> &out) const {
    const auto &n = node(id);
    if (n.kind() == NodeKind::read || n.kind() == NodeKind::place) {
      if (auto root = place_root(id))
        out.emplace_back(*root, std::vector<PathSegment>{});
      return;
    }
    if (n.kind() == NodeKind::field_read ||
        n.kind() == NodeKind::field_place) {
      auto root = place_root(id);
      if (root)
        out.emplace_back(*root, place_path(id));
      if (root)
        collect_place_index_paths(id, out);
      else
        collect_access_paths(n.children[0], out);
      return;
    }
    if (n.kind() == NodeKind::index_read ||
        n.kind() == NodeKind::index_place) {
      auto root = place_root(id);
      if (root) {
        out.emplace_back(*root, place_path(id));
        collect_place_index_paths(id, out);
      } else {
        for (auto c : n.children)
          collect_access_paths(c, out);
      }
      return;
    }
    if (auto m = std::get_if<Move>(&n.data); m && m->binding) {
      out.emplace_back(*m->binding, std::vector<PathSegment>{});
      return;
    }
    for (auto c : n.children)
      collect_access_paths(c, out);
  }
  void collect_place_index_paths(HirNodeId id,
                                 std::vector<PlaceAccess> &out) const {
    const auto &n = node(id);
    if (n.kind() == NodeKind::field_place ||
        n.kind() == NodeKind::field_read) {
      collect_place_index_paths(n.children[0], out);
      return;
    }
    if (n.kind() == NodeKind::index_place ||
        n.kind() == NodeKind::index_read) {
      collect_place_index_paths(n.children[0], out);
      collect_access_paths(n.children[1], out);
    }
  }
  // Whole local scalar bindings cannot alias each other. An inout parameter
  // is also unique because every caller is checked by this same rule. Shared
  // Vec/Slice handles copied through other bindings, fields, enum payloads or
  // closures are never assumed distinct merely because their SymbolIds differ.
  void check_call_aliases(const std::vector<CallArg> &args,
                          bool indirect_callee) const {
    std::vector<PlaceAccess> borrowed;
    bool container_borrow = false;
    for (const auto &a : args) {
      if (a.mode != ParamMode::inout)
        continue;
      auto root = place_root(a.expression);
      need(root.has_value(),
           "inout place has no binding-backed storage root");
      auto path = place_path(a.expression);
      container_borrow =
          container_borrow || !container_path(path).indexes.empty();
      for (const auto &prior : borrowed)
        need(!places_overlap(prior.first, prior.second, *root, path),
             "overlapping or possibly aliased inout storage in call");
      borrowed.emplace_back(*root, std::move(path));
    }
    if (container_borrow && indirect_callee)
      fail("indirect callee may retain or resize the borrowed Vec storage");
    auto check = [&](const std::vector<PlaceAccess> &accesses) {
      for (const auto &access : accesses)
        for (const auto &borrow : borrowed)
          need(!places_overlap(borrow.first, borrow.second, access.first,
                               access.second),
               "inout storage is also accessed by another argument");
    };
    std::vector<TypeId> risks;
    for (const auto &a : args) {
      if (a.mode == ParamMode::inout) {
        std::vector<PlaceAccess> indexes;
        collect_place_index_paths(a.expression, indexes);
        check(indexes);
        if (container_borrow)
          collect_place_index_risks(a.expression, risks);
      } else {
        std::vector<PlaceAccess> accesses;
        collect_access_paths(a.expression, accesses);
        check(accesses);
        if (container_borrow)
          collect_alias_risks(a.expression, risks);
      }
    }
    need(risks.empty(),
         "argument may alias a shared container while its element is borrowed "
         "by inout");
  }
  // A type parameter spelling is only a real bounded template `T` when it is
  // an actual parameter of the enclosing template (or that template's
  // instance origin) and that declaration carries a trait bound. `Self` in
  // an impl is registered as a type_parameter symbol too, but it belongs to
  // an ImplDecl self parameter and source_type restores it to impl.self_type.
  // It therefore follows the ordinary concrete Type.method rules, while a
  // real bounded T keeps the required trait/prototype/impl guard.
  bool template_trait_parameter(const SourceMethodCall &source,
                                SymbolId owner) const {
    if (!source.base_symbol) return false;
    auto id = *source.base_symbol;
    if (symbol(id).kind != modules::SymbolKind::type_parameter)
      return false;
    auto origin = owner;
    for (const auto &i : m.instances)
      if (i.symbol == owner) {
        origin = i.origin;
        break;
      }
    auto t = templates.find(origin.value);
    if (t == templates.end()) return false;
    for (const auto &p : t->second->params)
      if (p.symbol == id)
        return p.trait.has_value();
    return false;
  }
  struct ConstraintMethodArguments {
    std::vector<GenericParam> parameters;
    std::vector<TypeId> values;
  };
  // Recover the complete method-own actual arguments for a ConstraintCall.
  // An explicit suffix comes from the real Source and is substituted through
  // the owning function instance; without a suffix the already determined
  // signature must determine every own parameter. Phantom parameters cannot
  // be inferred and therefore require the explicit spelling.
  ConstraintMethodArguments
  constraint_method_arguments(const HirNode &n, const ConstraintCall &c,
                              const TraitDecl &tr,
                              const MethodPrototype &prototype) const {
    auto written = source_method_call(n);
    need(written.has_value(), "constraint call lost its source method");
    auto parameters = source_parameters(prototype.source);
    auto owner = scope(n.scope).function;
    Substitutions sub;
    sub.push_back({tr.self, c.receiver});
    need(c.trait.args.size() == tr.params.size(),
         "constraint call trait application arity");
    for (size_t i = 0; i < tr.params.size(); ++i)
      sub.push_back({tr.params[i].symbol, c.trait.args[i]});
    std::vector<TypeId> values;
    if (written->explicit_type_arguments) {
      need(written->explicit_arguments.size() == parameters.size(),
           "constraint method type argument count");
      for (size_t i = 0; i < parameters.size(); ++i) {
        auto t =
            source_type_in_function(written->explicit_arguments[i], owner);
        valid_type(t, allowed(owner));
        values.push_back(t);
        sub.push_back({parameters[i].symbol, t});
      }
      need(c.signature == substitute(prototype.signature, sub, substituted),
           "constraint method type arguments differ from signature");
    } else {
      Substitutions match = sub;
      need(matches(prototype.signature, c.signature, match),
           "constraint method signature does not determine type arguments");
      for (const auto &p : parameters) {
        auto it = std::find_if(match.begin(), match.end(), [&](auto entry) {
          return entry.first == p.symbol;
        });
        need(it != match.end(),
             "constraint method type argument is missing from source");
        values.push_back(it->second);
      }
    }
    constraints(parameters, values, &sub);
    return {std::move(parameters), std::move(values)};
  }
  // Concrete value/Type.method(...) and the concrete form of a T.method(...)
  // instance become ordinary direct Calls. Re-derive the qualifier's
  // real source type and require the selected method to belong to the impl
  // for that type. For a concrete qualifier, the same source selection must
  // still have exactly one candidate; a template qualifier is matched by the
  // required trait in instance_body instead, so same-named methods from other
  // traits cannot replace it there.
  void check_qualified_method_source(const HirNode &n, const Call &c) const {
    auto source = source_method_call(n);
    if (!source)
      return;
    const bool template_qualifier =
        template_trait_parameter(*source, scope(n.scope).function);
    const bool type_qualifier = source->base_symbol &&
        (template_qualifier ||
         symbol(*source->base_symbol).kind == modules::SymbolKind::type ||
         symbol(*source->base_symbol).kind == modules::SymbolKind::builtin_type ||
         symbol(*source->base_symbol).kind == modules::SymbolKind::type_parameter);
    need(c.kind == CalleeKind::direct && c.direct_symbol.has_value(),
         "type-qualified method source is not a direct call");
    auto origin = *c.direct_symbol;
    const FunctionInstance *instance = nullptr;
    for (const auto &i : m.instances)
      if (i.symbol == origin) {
        instance = &i;
        origin = i.origin;
        break;
      }
    need(symbol(origin).name == source->name,
         "type-qualified call target name differs from source");
    const ImplDecl *impl = nullptr;
    for (const auto &candidate : m.impls)
      if (std::find(candidate.methods.begin(), candidate.methods.end(),
                    origin) != candidate.methods.end()) {
        need(!impl, "type-qualified call target belongs to two impls");
        impl = &candidate;
      }
    if (!type_qualifier && (!impl || !impl->trait)) {
      if (receiver_input(source->base).second == ParamMode::ordinary)
        return;
      need(impl && inherent_self_source(origin, *impl),
           "explicit receiver requires the real inherent first Self");
      const auto &signature = type(function(origin).signature);
      need(!signature.parameters.empty(), "method receiver signature is empty");
      check_receiver_source(n, c.args, source->base,
                            signature.parameters.front().mode);
      return;
    }
    need(impl, "type-qualified call target is not an impl method");
    if (impl->trait) {
      const TraitDecl *trait = trait_declaration(impl->trait->declaration);
      need(trait, "type-qualified call impl has no real trait");
      const MethodPrototype *prototype = nullptr;
      for (const auto &p : trait->methods)
        if (symbol(p.symbol).name == source->name) {
          need(!prototype, "type-qualified call has duplicate trait prototype");
          prototype = &p;
        }
      need(prototype, "type-qualified call has no source trait prototype");
      if (!type_qualifier) {
        const auto &raw = type(prototype->signature);
        need(!raw.parameters.empty(),
             "value call requires a trait Self parameter");
        const auto &first = type(raw.parameters.front().type);
        need(first.kind == TypeKind::TypeParameter &&
                 first.declaration == trait->self,
             "value call first parameter must be the real trait Self");
        check_receiver_source(n, c.args, source->base,
                              raw.parameters.front().mode);
        return;
      }
    }
    std::vector<TypeId> args;
    if (instance)
      args = instance->type_args;
    else if (templates.count(origin.value))
      args = c.type_args;
    need(impl->params.size() <= args.size(),
         "type-qualified call lost impl type arguments");
    Substitutions sub;
    for (size_t i = 0; i < impl->params.size(); ++i)
      sub.push_back({impl->params[i].symbol, args[i]});
    const auto source_type =
        source_type_in_function(source->base, scope(n.scope).function);
    need(substitute(impl->self_type, sub, substituted) == source_type,
         "type-qualified call source type differs from impl Self");
    if (template_qualifier) {
      need(impl->trait.has_value(),
           "T-qualified call selected an inherent method");
      const TraitDecl *trait = trait_declaration(impl->trait->declaration);
      need(trait, "T-qualified call impl has no real trait");
      bool declared = false;
      for (const auto &p : trait->methods)
        declared |= symbol(p.symbol).name == source->name;
      need(declared, "T-qualified call has no source trait prototype");
    } else {
      size_t candidates = 0;
      for (const auto &candidate : m.impls) {
        Substitutions match;
        if (!matches(candidate.self_type, source_type, match))
          continue;
        bool valid = true;
        for (const auto &p : candidate.params) {
          auto it = std::find_if(match.begin(), match.end(),
                                 [&](auto x) { return x.first == p.symbol; });
          if (it == match.end()) {
            valid = false;
            break;
          }
          if (p.trait) {
            TraitRef bound = *p.trait;
            for (auto &arg : bound.args)
              arg = substitute(arg, match, substituted);
            if (!satisfies(it->second, bound)) {
              valid = false;
              break;
            }
          }
        }
        if (!valid)
          continue;
        for (auto method : candidate.methods)
          if (symbol(method).name == source->name)
            ++candidates;
      }
      need(candidates == 1,
           "type-qualified method source selection is not unique");
    }
  }
  // Explicit method type arguments are source data, not a signature cache.
  // Compare them with the complete instance key after the impl prefix. This
  // keeps same-signature phantom instances (for example Tag One vs Two)
  // distinguishable even though their checked signatures are identical.
  void check_explicit_method_arguments(const HirNode &n,
                                       const Call &c) const {
    auto written = source_method_call(n);
    if (!written || !written->explicit_type_arguments)
      return;
    need(c.kind == CalleeKind::direct && c.direct_symbol.has_value(),
         "explicit method type arguments require a direct call");
    auto origin = *c.direct_symbol;
    const FunctionInstance *instance = nullptr;
    for (const auto &i : m.instances)
      if (i.symbol == origin) {
        instance = &i;
        origin = i.origin;
        break;
      }
    const ImplDecl *impl = nullptr;
    for (const auto &candidate : m.impls)
      if (std::find(candidate.methods.begin(), candidate.methods.end(),
                    origin) != candidate.methods.end()) {
        need(!impl, "explicit method target belongs to two impls");
        impl = &candidate;
      }
    need(impl, "explicit method target is not an impl method");
    auto all = source_parameters(function(origin).source);
    need(all.size() >= impl->params.size(),
         "explicit method source lost its impl prefix");
    for (size_t i = 0; i < impl->params.size(); ++i)
      need(all[i].symbol == impl->params[i].symbol,
           "explicit method impl prefix differs from source");
    const size_t own = all.size() - impl->params.size();
    need(written->explicit_arguments.size() == own,
         "explicit method type argument count differs from declaration");
    std::vector<TypeId> actual;
    if (instance)
      actual = instance->type_args;
    else
      actual = c.type_args;
    need(actual.size() == all.size(),
         "explicit method call lost complete type arguments");
    auto owner = scope(n.scope).function;
    for (size_t i = 0; i < own; ++i) {
      auto t = source_type_in_function(written->explicit_arguments[i], owner);
      valid_type(t, allowed(owner));
      need(t == actual[impl->params.size() + i],
           "explicit method type argument differs from complete instance key");
    }
  }
  // The real bound of a bounded template qualifier, rebuilt from the owning
  // template's Source. An instance substitutes its actual type arguments.
  // This does not trust a producer-provided bound on T or borrow another T's
  // constraint.
  std::optional<TraitRef>
  source_template_bound(const SourceMethodCall &source, SymbolId owner,
                        SymbolId *parameter = nullptr) const {
    if (!source.base_symbol)
      return {};
    const auto id = *source.base_symbol;
    if (symbol(id).kind != modules::SymbolKind::type_parameter)
      return {};
    auto origin = owner;
    const FunctionInstance *instance = nullptr;
    for (const auto &i : m.instances)
      if (i.symbol == owner) {
        instance = &i;
        origin = i.origin;
        break;
      }
    auto found = templates.find(origin.value);
    if (found == templates.end())
      return {};
    auto real = source_parameters(found->second->function.source);
    const GenericParam *declared = nullptr;
    for (const auto &p : real)
      if (p.symbol == id)
        declared = &p;
    if (!declared || !declared->trait)
      return {};
    TraitRef bound = *declared->trait;
    if (instance) {
      need(real.size() == instance->type_args.size(),
           "template instance parameter count");
      Substitutions sub;
      for (size_t i = 0; i < real.size(); ++i)
        sub.push_back({real[i].symbol, instance->type_args[i]});
      for (auto &arg : bound.args)
        arg = substitute(arg, sub, substituted);
    }
    if (parameter)
      *parameter = id;
    return bound;
  }
  TypeId source_declaration_signature(SymbolId origin) const {
    auto template_ = templates.find(origin.value);
    if (template_ != templates.end())
      return template_->second->function.signature;
    return function(origin).signature;
  }
  const Source &source_declaration(SymbolId origin) const {
    auto template_ = templates.find(origin.value);
    if (template_ != templates.end())
      return template_->second->function.source;
    return function(origin).source;
  }
  struct MethodValueTarget {
    SymbolId origin;
    const FunctionInstance *instance = nullptr;
    std::vector<TypeId> complete_type_args;
    TypeId signature;
  };
  MethodValueTarget method_value_target(const HirNode &n,
                                        const FunctionValue &v) const {
    MethodValueTarget target;
    target.signature = n.type;
    if (templates.count(v.symbol.value)) {
      need(templates.count(scope(n.scope).function.value),
           "template method value survived into concrete execution");
      target.origin = v.symbol;
      target.complete_type_args = v.type_args;
      return target;
    }
    auto instance = instances.find(v.symbol.value);
    if (instance != instances.end()) {
      need(v.type_args.empty(), "instance method value has a type suffix");
      target.instance = instance->second;
      target.origin = target.instance->origin;
      target.complete_type_args = target.instance->type_args;
      return target;
    }
    need(v.type_args.empty(), "concrete method value has a type suffix");
    target.origin = v.symbol;
    return target;
  }
  const ImplDecl *method_impl(SymbolId origin) const {
    const ImplDecl *result = nullptr;
    for (const auto &im : m.impls)
      if (std::find(im.methods.begin(), im.methods.end(), origin) !=
          im.methods.end()) {
        need(!result, "method value target belongs to two impls");
        result = &im;
      }
    need(result, "method value target is not an impl method");
    return result;
  }
  bool impl_method_exists(SymbolId method) const {
    for (const auto &im : m.impls)
      if (std::find(im.methods.begin(), im.methods.end(), method) !=
          im.methods.end())
        return true;
    return false;
  }
  bool method_value_is_impl(const FunctionValue &v) const {
    auto origin = v.symbol;
    if (templates.count(v.symbol.value))
      origin = v.symbol;
    else if (auto i = instances.find(v.symbol.value); i != instances.end())
      origin = i->second->origin;
    return impl_method_exists(origin);
  }
  // A method value may be reached through one or more top-level constants.
  // Follow the real binding initializer AST; no function body or synthetic
  // symbol is created, and a cycle is rejected independently of sema's
  // constant evaluator.
  std::optional<SourceMethodCall>
  constant_method_source(const HirNode &n) const {
    auto constant = resolved_source(n.source.node);
    if (!constant ||
        symbol(*constant).kind != modules::SymbolKind::constant)
      return {};
    std::set<uint64_t> seen;
    for (;;) {
      need(seen.insert(constant->value).second,
           "constant method value alias cycle");
      const auto &s = symbol(*constant);
      need(s.definition.has_value(),
           "constant method value has no initializer");
      const auto &file =
          *m.resolved->project->module(s.definition->module)
               .files.at(s.definition->file);
      const auto &binding = file.node(s.definition->node);
      need(binding.kind == AstKind::Binding && !binding.children.empty(),
           "constant method value source is not a binding");
      auto initializer = *s.definition;
      initializer.node = binding.children.back();
      auto next = resolved_source(initializer);
      if (next && symbol(*next).kind == modules::SymbolKind::constant) {
        constant = next;
        continue;
      }
      return source_method_member(initializer);
    }
  }
  std::optional<std::vector<TypeId>>
  matched_impl_arguments(const ImplDecl &im,
                         const Substitutions &match) const {
    std::vector<TypeId> args;
    for (const auto &p : im.params) {
      auto it = std::find_if(match.begin(), match.end(), [&](auto entry) {
        return entry.first == p.symbol;
      });
      if (it == match.end())
        return std::nullopt;
      if (p.trait) {
        TraitRef bound = *p.trait;
        for (auto &arg : bound.args)
          arg = substitute(arg, match, substituted);
        if (!satisfies(it->second, bound))
          return std::nullopt;
      }
      args.push_back(it->second);
    }
    return args;
  }
  // The one source method candidate for a concrete qualifier. A required
  // trait is matched by Self and every ordered argument together; both forms
  // require a complete impl-parameter substitution.
  const ImplDecl *
  unique_source_method(TypeId receiver, const TraitRef *required,
                       std::string_view name, Substitutions &selected_sub,
                       std::vector<TypeId> &selected_args) const {
    const ImplDecl *selected = nullptr;
    for (const auto &im : m.impls) {
      if (required &&
          (!im.trait || im.trait->declaration != required->declaration))
        continue;
      Substitutions match;
      if (!matches(im.self_type, receiver, match))
        continue;
      if (required) {
        if (im.trait->args.size() != required->args.size())
          continue;
        bool matched = true;
        for (size_t a = 0; a < required->args.size(); ++a)
          if (!matches(im.trait->args[a], required->args[a], match)) {
            matched = false;
            break;
          }
        if (!matched)
          continue;
      }
      bool has_method = false;
      for (auto method : im.methods)
        if (symbol(method).name == name)
          has_method = true;
      if (!has_method)
        continue;
      auto args = matched_impl_arguments(im, match);
      if (!args)
        continue;
      need(!selected, "method value source selection is ambiguous");
      selected = &im;
      selected_sub = std::move(match);
      selected_args = std::move(*args);
    }
    need(selected, "method value source selection has no impl");
    return selected;
  }
  void check_method_visibility(SymbolId method, const ImplDecl &impl,
                               ModuleId module) const {
    if (symbol(method).module == module)
      return;
    need(impl.trait ? symbol(impl.trait->declaration).exported
                    : symbol(method).exported,
         "method value target is private");
  }
  // Verify one executable Type.method[U] value from its real Source and its
  // complete target. This rejects another same-named impl, another complete
  // trait application, a fabricated instance key, a changed signature/mode,
  // a missing phantom suffix, or a target prototype in concrete execution.
  void check_concrete_method_value_source(
      const HirNode &n, const FunctionValue &v,
      const SourceMethodCall &source,
      std::optional<ModuleId> source_module = {},
      std::optional<SymbolId> source_owner = {}) const {
    // A const initializer has no function instance environment. Use an empty
    // owner so source_type_in_function performs no template substitution;
    // top-level const types are already concrete.
    const auto owner = source_owner.value_or(scope(n.scope).function);
    const auto module = source_module.value_or(n.source.node.module);
    auto bounded = source_template_bound(source, owner);
    const bool type_parameter = source.base_symbol &&
        symbol(*source.base_symbol).kind ==
            modules::SymbolKind::type_parameter;
    need(source.base_symbol.has_value(), "method value lost its base symbol");
    need(type_parameter ? bounded.has_value()
                        : (symbol(*source.base_symbol).kind ==
                               modules::SymbolKind::type ||
                           symbol(*source.base_symbol).kind ==
                               modules::SymbolKind::builtin_type),
         "method value source is not a concrete Type or bounded T");
    const auto receiver = source_type_in_function(source.base, owner);
    if (bounded) {
      check_trait_application(*bounded, allowed(owner));
      check_trait_parameter_bounds(*bounded, receiver);
    }
    auto target = method_value_target(n, v);
    const auto *impl = method_impl(target.origin);
    need(symbol(target.origin).name == source.name,
         "method value target name differs from source");
    check_method_visibility(target.origin, *impl, module);
    Substitutions selected_sub;
    std::vector<TypeId> impl_args;
    const auto *selected = unique_source_method(
        receiver, bounded ? &*bounded : nullptr, source.name, selected_sub,
        impl_args);
    need(selected == impl,
         "method value source selected another impl than its target");

    auto declared = source_parameters(source_declaration(target.origin));
    need(declared.size() >= impl->params.size(),
         "method value source lost its impl generic prefix");
    for (size_t i = 0; i < impl->params.size(); ++i)
      need(declared[i].symbol == impl->params[i].symbol,
           "method value impl prefix differs from source");
    std::vector<GenericParam> own(declared.begin() + impl->params.size(),
                                  declared.end());
    const bool has_type_arguments =
        templates.count(v.symbol.value) || target.instance;
    if (has_type_arguments) {
      need(target.complete_type_args.size() == declared.size(),
           "method value complete type argument count");
      for (size_t i = 0; i < impl->params.size(); ++i)
        need(target.complete_type_args[i] == impl_args[i],
             "method value impl type argument differs from qualifier");
    } else {
      need(impl->params.empty() && own.empty() &&
               target.complete_type_args.empty(),
           "generic method value has no instance target");
    }
    std::vector<TypeId> own_args;
    if (has_type_arguments)
      own_args.assign(target.complete_type_args.begin() +
                          impl->params.size(),
                      target.complete_type_args.end());
    need(own_args.size() == own.size(),
         "method value method-own argument count");
    if (source.explicit_type_arguments) {
      need(source.explicit_arguments.size() == own.size(),
           "method value explicit type argument count");
      for (size_t i = 0; i < own.size(); ++i) {
        auto t = source_type_in_function(source.explicit_arguments[i], owner);
        valid_type(t, allowed(owner));
        need(t == own_args[i],
             "method value explicit type argument differs from target");
      }
    } else {
      Substitutions match;
      for (size_t i = 0; i < impl->params.size(); ++i)
        match.push_back({impl->params[i].symbol, impl_args[i]});
      need(matches(source_declaration_signature(target.origin),
                   target.signature, match),
           "method value signature does not determine its own arguments");
      for (size_t i = 0; i < own.size(); ++i) {
        auto it = std::find_if(match.begin(), match.end(), [&](auto entry) {
          return entry.first == own[i].symbol;
        });
        need(it != match.end(),
             "method value phantom argument requires an explicit suffix");
        need(it->second == own_args[i],
             "method value inferred own argument differs from target");
      }
    }
    Substitutions outer;
    for (size_t i = 0; i < impl->params.size(); ++i)
      outer.push_back({impl->params[i].symbol, impl_args[i]});
    constraints(own, own_args, &outer);

    if (impl->trait) {
      const auto *trait = trait_declaration(impl->trait->declaration);
      need(trait, "method value impl has no real trait");
      const MethodPrototype *prototype = nullptr;
      for (const auto &p : trait->methods)
        if (symbol(p.symbol).name == source.name) {
          need(!prototype, "method value has duplicate trait prototype");
          prototype = &p;
        }
      need(prototype, "method value has no trait prototype");
      TraitRef application = *impl->trait;
      for (auto &arg : application.args)
        arg = substitute(arg, selected_sub, substituted);
      if (bounded)
        need(application == *bounded,
             "method value full trait application differs from its bound");
      auto prototype_own = source_parameters(prototype->source);
      need(prototype_own.size() == own.size(),
           "method value prototype own count");
      Substitutions alpha{{trait->self, receiver}};
      for (size_t i = 0; i < trait->params.size(); ++i)
        alpha.push_back({trait->params[i].symbol, application.args[i]});
      for (size_t i = 0; i < prototype_own.size(); ++i)
        alpha.push_back({prototype_own[i].symbol, own_args[i]});
      need(substitute(prototype->signature, alpha, substituted) ==
               target.signature,
           "method value prototype signature differs from target");
    } else
      need(!bounded, "bounded T method value selected an inherent method");
    need(n.type == target.signature, "method value type mismatch");
  }
  // The strict template exception: only a real bounded T method of this
  // owning template may name a raw trait prototype. Its type_args are the
  // ordered method-own arguments; the qualifier, complete bound and prototype
  // are rebuilt from Source. No executable function is fabricated.
  void check_template_method_value_source(
      const HirNode &n, const FunctionValue &v,
      const TraitDecl &trait, const MethodPrototype &prototype,
      const SourceMethodCall &source) const {
    const auto owner = scope(n.scope).function;
    need(templates.count(owner.value),
         "trait prototype method value is outside a template");
    SymbolId parameter{};
    auto bounded = source_template_bound(source, owner, &parameter);
    need(bounded.has_value(), "trait prototype method value has no bound");
    need(source.base_symbol && *source.base_symbol == parameter,
         "trait prototype method value base differs from its parameter");
    need(trait.symbol == bounded->declaration,
         "trait prototype belongs to another bound");
    need(v.symbol == prototype.symbol,
         "trait prototype method value target differs from source");
    need(symbol(prototype.symbol).name == source.name,
         "trait prototype method value name differs from source");
    const auto qualifier = source_type_in_function(source.base, owner);
    const auto &q = type(qualifier);
    need(q.kind == TypeKind::TypeParameter && q.declaration == parameter,
         "trait prototype qualifier is not the bounded T");
    check_trait_application(*bounded, allowed(owner));
    check_trait_parameter_bounds(*bounded, qualifier);
    auto own = source_parameters(prototype.source);
    need(v.type_args.size() == own.size(),
         "trait prototype method-own argument count");
    if (source.explicit_type_arguments) {
      need(source.explicit_arguments.size() == own.size(),
           "trait prototype explicit type argument count");
      for (size_t i = 0; i < own.size(); ++i) {
        auto t = source_type_in_function(source.explicit_arguments[i], owner);
        valid_type(t, allowed(owner));
        need(t == v.type_args[i],
             "trait prototype explicit type argument differs from target");
      }
    } else {
      Substitutions match{{trait.self, qualifier}};
      for (size_t i = 0; i < trait.params.size(); ++i)
        match.push_back({trait.params[i].symbol, bounded->args[i]});
      need(matches(prototype.signature, n.type, match),
           "trait prototype signature does not determine its own arguments");
      for (size_t i = 0; i < own.size(); ++i) {
        auto it = std::find_if(match.begin(), match.end(), [&](auto entry) {
          return entry.first == own[i].symbol;
        });
        need(it != match.end(),
             "trait prototype phantom argument requires an explicit suffix");
        need(it->second == v.type_args[i],
             "trait prototype inferred argument differs from target");
      }
    }
    Substitutions alpha{{trait.self, qualifier}};
    for (size_t i = 0; i < trait.params.size(); ++i)
      alpha.push_back({trait.params[i].symbol, bounded->args[i]});
    for (size_t i = 0; i < own.size(); ++i)
      alpha.push_back({own[i].symbol, v.type_args[i]});
    constraints(own, v.type_args, &alpha);
    need(n.type == substitute(prototype.signature, alpha, substituted),
         "trait prototype method value signature mismatch");
    need(!functions.count(v.symbol.value) &&
             !templates.count(v.symbol.value),
         "trait prototype method value has a fake executable body");
  }
  void check_call(const HirNode &n, const Call &c) const {
    need((c.kind == CalleeKind::system) == c.system.has_value(),
         "system operation metadata on wrong call kind");
    if (c.kind == CalleeKind::foreign) {
      foreign_call(n, c);
      return;
    }
    if (c.kind == CalleeKind::c_foreign) {
      c_foreign_call(n, c);
      return;
    }
    need(!c.binding && !c.c_callable && c.omitted.empty(),
         "foreign metadata on ordinary call");
    for (auto argument : c.type_args)
      valid_type(argument, allowed(scope(n.scope).function));
    const auto &sig = type(c.signature);
    need(sig.kind == TypeKind::Function, "call signature kind");
    need(n.type == sig.result, "call result mismatch");
    const HirFunction *direct = nullptr;
    size_t offset = 0;
    if (c.kind == CalleeKind::direct) {
      need(c.direct_symbol.has_value() && !c.callee, "direct callee metadata");
      if (!templates.count(scope(n.scope).function.value))
        need(!templates.count(c.direct_symbol->value) && c.type_args.empty(),
             "template call survived into concrete execution");
      direct = &function(*c.direct_symbol);
      need(instantiated_signature(*c.direct_symbol, c.type_args) == c.signature,
           "direct signature mismatch");
    } else if (c.kind == CalleeKind::function_value) {
      need(c.type_args.empty(), "type arguments on indirect call");
      need(!c.direct_symbol && c.callee.has_value(),
           "function value callee metadata");
      need(expr(*c.callee).type == c.signature,
           "function value signature mismatch");
      offset = 1;
    } else if (c.kind == CalleeKind::system) {
      const auto *spec = system_operation_spec(*c.system);
      need(spec && c.direct_symbol && !c.callee,
           "system operation target metadata");
      const size_t type_arg_count = spec->type_arity;
      need(c.type_args.size() == type_arg_count,
           "system operation type argument count");
      const auto &target = symbol(*c.direct_symbol);
      need(target.kind == modules::SymbolKind::builtin_function &&
               target.name == spec->name && !target.definition &&
               !target.declaration && !target.exported && !target.target && !target.foreign,
           "system operation target is not the real builtin");
      need(std::count_if(m.resolved->symbols.begin(), m.resolved->symbols.end(),
                        [&](const auto &s) {
                          return s.kind == modules::SymbolKind::builtin_function &&
                                 s.name == spec->name;
                        }) == 1,
           "system operation builtin identity is ambiguous");
      const auto &mod = m.resolved->project->module(n.source.node.module);
      need(mod.logical_path == spec->owner && !mod.binding,
           std::string("system operation source is outside ") +
               std::string(spec->owner));
      need(c.signature ==
               system_operation_signature(substituted, m, *c.system, c.type_args),
           "system operation fixed signature mismatch");
      const auto &file = *mod.files.at(n.source.node.file);
      const auto &call = file.node(n.source.node.node);
      need(call.kind == AstKind::Call && call.children.size() == spec->count + 1 &&
               c.args.size() == spec->count,
           "system operation source call/arity");
      auto callee = n.source.node;
      callee.node = call.children.front();
      while (file.node(callee.node).kind == AstKind::Parenthesized)
        callee.node = file.node(callee.node).children.at(0);
      const auto &syntax_target = file.node(callee.node);
      if (type_arg_count) {
        need(syntax_target.kind == AstKind::NeutralBracket &&
                 syntax_target.children.size() == type_arg_count + 1,
             "system operation source type argument shape");
        auto base = callee;
        base.node = syntax_target.children.front();
        while (file.node(base.node).kind == AstKind::Parenthesized)
          base.node = file.node(base.node).children.at(0);
        const auto &syntax_base = file.node(base.node);
        need(syntax_base.kind == AstKind::Name &&
                 file.tokens.at(syntax_base.token_begin).spelling == spec->name &&
                 resolved_source(base) == c.direct_symbol,
             "system operation differs from resolved source builtin");
        for (size_t i = 0; i < type_arg_count; ++i) {
          auto source_argument = callee;
          source_argument.node = syntax_target.children[i + 1];
          auto source_type = source_type_in_function(source_argument, scope(n.scope).function);
          valid_type(source_type, allowed(scope(n.scope).function));
          need(source_type == c.type_args[i],
               "system operation source type argument differs from Call");
        }
      } else {
        need(syntax_target.kind == AstKind::Name &&
                 file.tokens.at(syntax_target.token_begin).spelling == spec->name &&
                 resolved_source(callee) == c.direct_symbol,
             "system operation differs from resolved source builtin");
      }
      if (*c.system == SystemOperation::weak_empty ||
          *c.system == SystemOperation::weak_downgrade ||
          *c.system == SystemOperation::weak_upgrade)
        need(c.type_args.size() == 1 &&
                 (type_contains_parameter(m, c.type_args[0]) ||
                  weak_target(m, c.type_args[0])),
             "Weak operation target is not a canonical source declaration");
      for (size_t i = 0; i < c.args.size(); ++i) {
        const auto &argument = file.node(call.children[i + 1]);
        need(argument.kind == AstKind::Argument && argument.children.size() == 1 &&
                 file.tokens.at(argument.token_begin + 1).spelling != ":" &&
                 !c.args[i].label && c.args[i].mode == ParamMode::ordinary,
             "system operation source argument label/mode");
        auto ref = n.source.node;
        ref.node = argument.children.front();
        ref = unwrapped(ref);
        const auto &value = file.node(ref.node);
        need(!(value.kind == AstKind::Unary &&
               (file.tokens.at(value.token_begin).spelling == "move" ||
                file.tokens.at(value.token_begin).spelling == "inout")),
             "system operation source argument marker");
        need(expr(c.args[i].expression).kind() != NodeKind::move &&
                 same_ref(unwrapped(expr(c.args[i].expression).source.node), ref),
             "system operation argument differs from source");
      }
    } else
      fail("unsupported callee kind");
    need(c.args.size() == sig.parameters.size() &&
             n.children.size() == c.args.size() + offset,
         "call arity/children");
    if (offset)
      need(n.children[0] == *c.callee, "callee must be evaluated first");
    if (c.kind == CalleeKind::function_value) {
      const auto &file =
          *m.resolved->project->module(n.source.node.module)
               .files.at(n.source.node.file);
      const auto &call = file.node(n.source.node.node);
      need(call.kind == AstKind::Call && !call.children.empty() &&
               call.children.size() == c.args.size() + 1,
           "function value call lost source call/arity");
      auto target = n.source.node;
      target.node = call.children.front();
      need(same_ref(unwrapped(node(*c.callee).source.node),
                    unwrapped(target)),
           "function value callee differs from source");
      for (size_t i = 0; i < c.args.size(); ++i) {
        const auto &syntax = file.node(call.children[i + 1]);
        need(syntax.kind == AstKind::Argument && syntax.children.size() == 1,
             "function value source argument kind");
        const auto &original = file.node(syntax.children[0]);
        const auto &value = node(c.args[i].expression).source;
        need(value.node.module == n.source.node.module &&
                 value.node.file == n.source.node.file &&
                 value.span.begin_byte >= original.span.begin_byte &&
                 value.span.end_byte <= original.span.end_byte,
             "function value argument differs from source");
      }
    }
    std::vector<bool> seen(sig.parameters.size());
    const auto owner_allowed = allowed(scope(n.scope).function);
    bool named = false;
    for (size_t i = 0; i < c.args.size(); ++i) {
      check_cancelled(stop);
      const auto &a = c.args[i];
      need(a.source_index == i && a.parameter_index < seen.size(),
           "call source/parameter index");
      need(!seen[a.parameter_index], "duplicate call parameter");
      seen[a.parameter_index] = true;
      need(n.children[i + offset] == a.expression,
           "call children must preserve source order");
      auto p = sig.parameters[a.parameter_index];
      const auto &argument = node(a.expression);
      need(a.mode == p.mode && argument.type == p.type,
           "call argument mode/type");
      if (p.mode == ParamMode::inout) {
        need(inout_place(argument) &&
                 inout_slot_type(m, p.type, owner_allowed),
             "inout requires a whole Woozy value place");
      } else {
        need(p.mode == ParamMode::ordinary || p.mode == ParamMode::move,
             "unsupported argument mode");
        expr(a.expression);
      }
      if (a.label) {
        named = true;
        need(direct && !a.label->empty() &&
                 *a.label == direct->parameters[a.parameter_index].label,
             "call label mismatch or label on function value");
      } else {
        need(!named && a.parameter_index == i,
             "positional argument index/order");
      }
    }
    check_call_aliases(c.args, c.callee.has_value());
    if (c.kind == CalleeKind::direct) {
      check_qualified_method_source(n, c);
      check_explicit_method_arguments(n, c);
    }
  }
  void trait_object_shape(const HirNode &n) const {
    need(n.children.size() == 1, "trait object construction arity");
    const auto &init = std::get<TraitObjectInit>(n.data);
    const auto &want = type(n.type);
    need(want.kind == TypeKind::TraitObject,
         "trait object construction destination is not dyn");
    need(init.trait.declaration == want.declaration &&
             init.trait.args == want.type_args,
         "trait object construction application differs from its type");
    const auto *tr = trait_declaration(init.trait.declaration);
    need(tr != nullptr, "trait object construction has no trait declaration");
    check_object_safe(init.trait.declaration, n.source.span);
    const auto &concrete = expr(n.children[0]);
    need(init.concrete == concrete.type &&
             type(concrete.type).kind != TypeKind::TraitObject &&
             !type_contains_parameter(m, concrete.type),
         "trait object construction concrete type mismatch");
    need(init.methods.size() == tr->methods.size(),
         "trait object vtable method count");
    const ImplDecl *impl = nullptr;
    for (const auto &candidate : m.impls) {
      if (!candidate.trait ||
          candidate.trait->declaration != init.trait.declaration ||
          candidate.trait->args.size() != init.trait.args.size())
        continue;
      Substitutions sub;
      if (!matches(candidate.self_type, concrete.type, sub))
        continue;
      bool matched = true;
      for (size_t i = 0; i < init.trait.args.size(); ++i)
        if (!matches(candidate.trait->args[i], init.trait.args[i], sub)) {
          matched = false;
          break;
        }
      if (!matched)
        continue;
      need(impl == nullptr, "ambiguous trait object implementation");
      impl = &candidate;
    }
    need(impl != nullptr, "trait object construction has no implementation");
    for (size_t i = 0; i < tr->methods.size(); ++i) {
      const auto &proto = tr->methods[i];
      auto target = init.methods[i];
      SymbolId origin = target;
      for (const auto &instance : m.instances)
        if (instance.symbol == target)
          origin = instance.origin;
      std::optional<SymbolId> expected;
      for (auto method : impl->methods)
        if (symbol(method).name == symbol(proto.symbol).name) {
          need(!expected, "duplicate trait object implementation method");
          expected = method;
        }
      need(expected && origin == *expected,
           "trait object method target differs from the unique implementation");
      (void)function(target);
      need(!templates.count(target.value),
           "trait object method target is not concrete");
      need(init.method_args.size() == init.methods.size(),
           "trait object method argument metadata");
      for (const auto &instance : m.instances)
        if (instance.symbol == target)
          need(instance.type_args == init.method_args[i],
               "trait object method instance arguments differ");
    }
  }
  void trait_dispatch_shape(const HirNode &n) const {
    const auto &dispatch = std::get<TraitDispatch>(n.data);
    need(!n.children.empty(), "trait dispatch has no receiver");
    const auto &receiver_type = type(expr(n.children[0]).type);
    need(receiver_type.kind == TypeKind::TraitObject,
         "trait dispatch receiver is not dyn");
    need(dispatch.trait.declaration == receiver_type.declaration &&
             dispatch.trait.args == receiver_type.type_args,
         "trait dispatch application differs from its receiver");
    const auto *tr = trait_declaration(dispatch.trait.declaration);
    need(tr != nullptr, "trait dispatch has no trait declaration");
    check_object_safe(dispatch.trait.declaration, n.source.span);
    need(dispatch.index < tr->methods.size(),
         "trait dispatch vtable index");
    const auto &proto = tr->methods[dispatch.index];
    need(proto.symbol == dispatch.method,
         "trait dispatch vtable method differs from its prototype");
    Substitutions sub;
    Type self_type;
    self_type.kind = TypeKind::TraitObject;
    self_type.declaration = receiver_type.declaration;
    self_type.type_args = receiver_type.type_args;
    auto receiver_id = substituted.intern(std::move(self_type));
    sub.push_back({tr->self, receiver_id});
    for (size_t i = 0; i < tr->params.size(); ++i)
      sub.push_back({tr->params[i].symbol, dispatch.trait.args[i]});
    const auto full = substitute(proto.signature, sub, substituted);
    const auto &full_type = type(full);
    need(full_type.kind == TypeKind::Function && !full_type.throws_type,
         "trait dispatch prototype is not a nonthrowing function");
    Type abi;
    abi.kind = TypeKind::Function;
    abi.result = full_type.result;
    for (size_t i = 1; i < full_type.parameters.size(); ++i)
      abi.parameters.push_back(full_type.parameters[i]);
    const auto abi_id = substituted.intern(std::move(abi));
    need(dispatch.signature == abi_id,
         "trait dispatch ABI signature mismatch");
    const auto &abi_type = type(dispatch.signature);
    need(n.type == abi_type.result, "trait dispatch result type");
    need(dispatch.args.size() == n.children.size() &&
             dispatch.args.size() == full_type.parameters.size(),
         "trait dispatch argument count");
    const auto &receiver = dispatch.args[0];
    need(receiver.source_index == 0 && receiver.parameter_index == 0 &&
             !receiver.label && receiver.mode == ParamMode::ordinary &&
             receiver.expression == n.children[0],
         "trait dispatch receiver mapping");
    bool named = false;
    std::set<uint32_t> seen;
    for (size_t i = 0; i < dispatch.args.size(); ++i) {
      const auto &a = dispatch.args[i];
      need(a.source_index == i && a.expression == n.children[i] &&
               a.parameter_index < full_type.parameters.size() &&
               seen.insert(a.parameter_index).second &&
               a.mode == ParamMode::ordinary,
           "trait dispatch argument mapping/mode");
      if (i > 0) {
        need(node(a.expression).type ==
                 full_type.parameters[a.parameter_index].type,
             "trait dispatch argument type");
        expr(a.expression);
      }
      if (a.label) {
        named = true;
        need(*a.label == proto.parameters[a.parameter_index].label,
             "trait dispatch argument label");
      } else {
        need(!named && a.parameter_index == i,
             "trait dispatch positional argument order");
      }
    }
  }
  void shape(HirNodeId id) const {
    check_cancelled(stop);
    const auto &n = node(id);
    need(!n.data.valueless_by_exception(), "valueless HIR payload");
    source(n.source);
    valid_type(n.type, allowed(scope(n.scope).function));
    need(type(n.type).kind != TypeKind::ForeignCallable || n.kind() == NodeKind::foreign_read,
         "internal foreign callable used as a source value");
    const auto &ch = n.children;
    auto arity = [&](size_t count) {
      need(ch.size() == count, "node child count");
    };
    switch (n.kind()) {
    case NodeKind::unit_literal:
      arity(0);
      unit(n);
      break;
    case NodeKind::bool_literal:
      arity(0);
      need(type(n.type).kind == TypeKind::Bool, "bool literal type");
      break;
    case NodeKind::integer_literal:
      arity(0);
      integer_literal(n, std::get<IntegerLiteral>(n.data));
      break;
    case NodeKind::float_literal:
      arity(0);
      need(type(n.type).kind == TypeKind::Float, "float literal type");
      need(type(n.type).bits != 32 ||
               std::get<FloatLiteral>(n.data).bits <= UINT32_MAX,
           "f32 literal bit pattern");
      break;
    case NodeKind::string_literal:
      arity(0);
      need(type(n.type).kind == TypeKind::String, "string literal type");
      try {
        validate_utf8(std::get<StringLiteral>(n.data).utf8);
      } catch (const SourceError &) {
        fail("invalid UTF-8 string");
      }
      need(std::get<StringLiteral>(n.data).utf8 == source_string(n.source.node),
           "string literal bytes differ from real source");
      break;
    case NodeKind::constant_literal:
      arity(0);
      need(std::get<ConstantLiteral>(n.data).value.type == n.type,
           "constant root type differs from expression");
      break;
    case NodeKind::read:
    case NodeKind::place:
      arity(0);
      break; // binding checks after tree traversal
    case NodeKind::function_value: {
      const auto &v = std::get<FunctionValue>(n.data);
      need(!(v.bound && !v.captures.empty()),
           "function value has both captures and a bound receiver");
      if (v.bound) {
        arity(1);
        need(expr(ch[0]).type == v.bound->type,
             "bound receiver child type");
      } else {
        arity(v.captures.size());
        for (size_t i = 0; i < ch.size(); ++i)
          need(expr(ch[i]).type == v.captures[i].type,
               "capture child type");
      }
      for (auto argument : v.type_args)
        valid_type(argument, allowed(scope(n.scope).function));
      if (trait_prototype(v.symbol)) {
        need(templates.count(scope(n.scope).function.value),
             "trait prototype function value survived into concrete execution");
      } else {
        if (!templates.count(scope(n.scope).function.value))
          need(!templates.count(v.symbol.value) && v.type_args.empty(),
               "template function value survived into concrete execution");
        const auto full = type(instantiated_signature(v.symbol, v.type_args));
        if (v.bound) {
          need(!full.parameters.empty(), "bound method has no Self parameter");
          Type trimmed = full;
          trimmed.parameters.erase(trimmed.parameters.begin());
          need(type(n.type) == trimmed, "bound method value type mismatch");
        } else {
          need(n.type == instantiated_signature(v.symbol, v.type_args),
               "function value type mismatch");
        }
      }
      break;
    }
    case NodeKind::binding: {
      arity(1);
      unit(n);
      const auto &s = symbol(std::get<Binding>(n.data).symbol);
      need(s.kind == modules::SymbolKind::value ||
               s.kind == modules::SymbolKind::constant,
           "binding symbol kind");
      expr(ch[0]);
      break;
    }
    case NodeKind::assign: {
      arity(2);
      unit(n);
      const auto &lhs = node(ch[0]);
      const auto &rhs = expr(ch[1]);
      need(lhs.kind() == NodeKind::place ||
               lhs.kind() == NodeKind::field_place ||
               lhs.kind() == NodeKind::index_place ||
               foreign_access_place(lhs),
           "assignment requires writable place");
      auto op = std::get<Assign>(n.data).compound;
      if (op) {
        need(*op <= BinaryOp::bit_xor, "invalid compound assignment operator");
        if (type(lhs.type).kind == TypeKind::String)
          need(lhs.kind() == NodeKind::place,
               "string compound assignment requires a whole binding");
        operator_types(*op, lhs.type, rhs.type, lhs.type);
      } else
        need(lhs.type == rhs.type, "assignment type mismatch");
      break;
    }
    case NodeKind::unary: {
      arity(1);
      const auto &a = expr(ch[0]);
      auto k = type(a.type).kind;
      switch (std::get<Unary>(n.data).op) {
      case UnaryOp::negate:
        need(number(k) && n.type == a.type, "negation types");
        break;
      case UnaryOp::logical_not:
        need(k == TypeKind::Bool && n.type == a.type, "logical not types");
        break;
      case UnaryOp::bit_not:
        need(integer(k) && n.type == a.type, "bitwise not types");
        break;
      default:
        fail("unknown unary operator");
      }
      break;
    }
    case NodeKind::binary:
      arity(2);
      operator_types(std::get<Binary>(n.data).op, expr(ch[0]).type,
                     expr(ch[1]).type, n.type);
      break;
    case NodeKind::short_circuit: {
      arity(2);
      auto op = std::get<ShortCircuit>(n.data).op;
      need(op == LogicalOp::and_ || op == LogicalOp::or_,
           "unknown short circuit operator");
      need(type(n.type).kind == TypeKind::Bool && expr(ch[0]).type == n.type &&
               expr(ch[1]).type == n.type,
           "short circuit types");
      break;
    }
    case NodeKind::integer_convert: {
      arity(1);
      // One shared record covers the allowed numeric pairs. Every f32/f64
      // source-target pair is valid: identity/widening are exact and
      // f64->f32 narrowing rounds once with IEEE RNE, overflowing to a signed
      // infinity. Bool and non-numeric sources/destinations stay invalid.
      const auto &source_type = type(expr(ch[0]).type);
      const auto &destination_type = type(n.type);
      const auto source = source_type.kind;
      const auto destination = destination_type.kind;
      const bool float_pair =
          source == TypeKind::Float && destination == TypeKind::Float &&
          (source_type.bits == 32 || source_type.bits == 64) &&
          (destination_type.bits == 32 || destination_type.bits == 64);
      need((integer(source) && integer(destination)) ||
               (source == TypeKind::Float && integer(destination)) ||
               (integer(source) && destination == TypeKind::Float) ||
               float_pair,
           "integer conversion types");
      // Reconstruct the written T(x) form. A same-shaped pair is not enough:
      // the callee must be the builtin numeric type in the result slot and the
      // operand must be the real, uniquely labelled-free argument syntax.
      const auto &file = *m.resolved->project->module(n.source.node.module)
                              .files.at(n.source.node.file);
      auto r = n.source.node;
      const auto &call = file.node(r.node);
      need(call.kind == AstKind::Call && call.children.size() == 2 &&
               n.children.size() == 1,
           "conversion source is not a real one-argument call");
      auto callee_ref = r;
      callee_ref.node = call.children[0];
      const auto callee = unwrapped(callee_ref);
      auto target_symbol = resolved_source(callee);
      need(target_symbol.has_value() &&
               symbol(*target_symbol).kind == modules::SymbolKind::builtin_type &&
               file.node(callee.node).children.empty(),
           "conversion callee is not a plain builtin numeric type");
      need(source_type_in_function(callee_ref, scope(n.scope).function) ==
               n.type,
           "conversion target differs from source callee");
      auto argument_ref = r;
      argument_ref.node = call.children[1];
      const auto &argument = file.node(argument_ref.node);
      need(argument.kind == AstKind::Argument && argument.children.size() == 1,
           "conversion source argument shape");
      need(!(argument.token_begin + 1 < argument.token_end &&
             file.tokens.at(argument.token_begin + 1).spelling == ":"),
           "conversion source argument has a label");
      auto operand_ref = argument_ref;
      operand_ref.node = argument.children[0];
      need(same_ref(unwrapped(node(ch[0]).source.node), unwrapped(operand_ref)),
           "conversion operand differs from the real source argument");
      need(same_span(node(ch[0]).source.span,
                     file.node(unwrapped(operand_ref).node).span),
           "conversion operand span differs from the complete source argument");
      break;
    }
    case NodeKind::call:
      check_call(n, std::get<Call>(n.data));
      break;
    case NodeKind::trait_object_init:
      trait_object_shape(n);
      break;
    case NodeKind::trait_dispatch:
      trait_dispatch_shape(n);
      break;
    case NodeKind::block:
      unit(n);
      for (auto c : ch)
        need(statement(node(c)), "block child must be a statement");
      break;
    case NodeKind::expression_statement:
      arity(1);
      unit(n);
      expr(ch[0]);
      break;
    case NodeKind::if_:
      need(ch.size() == 2 || ch.size() == 3, "if child count");
      unit(n);
      need(type(expr(ch[0]).type).kind == TypeKind::Bool &&
               node(ch[1]).kind() == NodeKind::block &&
               (ch.size() == 2 || node(ch[2]).kind() == NodeKind::block),
           "if condition/branch shape");
      break;
    case NodeKind::for_: {
      arity(4);
      unit(n);
      auto init = node(ch[0]).kind(), step = node(ch[2]).kind();
      need((init == NodeKind::binding || init == NodeKind::assign ||
            init == NodeKind::expression_statement || init == NodeKind::noop) &&
               (step == NodeKind::assign ||
                step == NodeKind::expression_statement ||
                step == NodeKind::noop) &&
               type(expr(ch[1]).type).kind == TypeKind::Bool &&
               node(ch[3]).kind() == NodeKind::block,
           "for children shape");
      break;
    }
    case NodeKind::noop:
    case NodeKind::break_:
    case NodeKind::continue_:
      arity(0);
      unit(n);
      break;
    case NodeKind::return_:
      need(ch.size() <= 1, "return child count");
      unit(n);
      if (!ch.empty())
        expr(ch[0]);
      break;
    case NodeKind::struct_init:
    case NodeKind::enum_init:
    case NodeKind::field_read:
    case NodeKind::field_place:
    case NodeKind::switch_:
    case NodeKind::constraint_call:
      aggregate_shape(n);
      break;
    case NodeKind::try_:
    case NodeKind::defer_:
    case NodeKind::throw_:
      effect_shape(n);
      break;
    case NodeKind::move: {
      const auto &mv = std::get<Move>(n.data);
      need(mv.binding ? ch.empty() : ch.size() == 1, "move source arity");
      if (!mv.binding)
        need(expr(ch[0]).type == n.type, "move temporary type");
      break;
    }
    case NodeKind::vec_init:
      need(type(n.type).kind == TypeKind::Vec, "list type must be Vec");
      for (auto c : ch)
        need(expr(c).type == type(n.type).type_args[0], "Vec element type");
      break;
    case NodeKind::index_read:
    case NodeKind::index_place: {
      arity(2);
      auto receiver = type(expr(ch[0]).type);
      bool text = receiver.kind == TypeKind::String;
      const bool c_pointer = receiver.kind == TypeKind::CForeignPointer ||
                             receiver.kind == TypeKind::CForeignBuffer;
      need((receiver.kind == TypeKind::Vec ||
            receiver.kind == TypeKind::Slice ||
            (text && n.kind() == NodeKind::index_read) || c_pointer) &&
               integer(type(expr(ch[1]).type).kind), "index types");
      if (c_pointer) {
        need(receiver.type_args.size() == 1 && n.type == receiver.type_args[0] &&
                 type(n.type).kind != TypeKind::Unit &&
                 (n.kind() != NodeKind::index_place || receiver.bits == 1),
             "C index result/write permission");
      } else {
        need(text ? type(n.type).kind == TypeKind::UnsignedInt && type(n.type).bits == 32
                  : n.type == receiver.type_args[0], "indexed element type");
      }
      const auto &file = *m.resolved->project->module(n.source.node.module)
                              .files.at(n.source.node.file);
      const auto &syntax = file.node(n.source.node.node);
      need(syntax.kind == AstKind::NeutralBracket &&
               syntax.children.size() == 2,
           "index source is not a real NeutralBracket");
      auto source_child = [&](size_t index) {
        auto ref = n.source.node;
        ref.node = syntax.children[index];
        return unwrapped(ref);
      };
      need(same_ref(unwrapped(node(ch[0]).source.node), source_child(0)) &&
               same_ref(unwrapped(node(ch[1]).source.node), source_child(1)),
           "index receiver/index differs from the real Bracket children");
      break;
    }
    case NodeKind::vec_slice: {
      const auto &sl = std::get<VecSlice>(n.data);
      need(ch.size() == 1 + bool(sl.start) + bool(sl.end), "slice arity");
      need((type(n.type).kind == TypeKind::Vec ||
            type(n.type).kind == TypeKind::Slice ||
            type(n.type).kind == TypeKind::String) &&
               expr(ch[0]).type == n.type, "slice receiver type");
      size_t i = 1;
      if (sl.start)
        need(ch[i++] == *sl.start && integer(type(expr(*sl.start).type).kind),
             "slice start");
      if (sl.end)
        need(ch[i++] == *sl.end && integer(type(expr(*sl.end).type).kind),
             "slice end");
      break;
    }
    case NodeKind::for_in: {
      arity(2);
      unit(n);
      const auto &fi = std::get<ForIn>(n.data);
      need((type(expr(ch[0]).type).kind == TypeKind::Vec ||
            type(expr(ch[0]).type).kind == TypeKind::Slice ||
            type(expr(ch[0]).type).kind == TypeKind::String) && fi.body == ch[1] &&
               node(fi.body).kind() == NodeKind::block &&
               node(fi.body).scope == fi.body_scope,
           "for-in type/body");
      declaration(fi.element, n.source, modules::SymbolKind::value,
                  AstKind::ForIn);
      break;
    }
    case NodeKind::foreign_read:
      if (std::get<ForeignRead>(n.data).access)
        foreign_access(id);
      else
        foreign_read(id);
      break;
    case NodeKind::c_field_read:
      c_field_read(n);
      break;
    case NodeKind::await_: {
      arity(1);
      const auto &task = type(expr(ch[0]).type);
      need(task.kind == TypeKind::Task, "await child is not a Task");
      need(n.type == task.type_args.at(0), "await result type");
      const auto error = task.type_args.at(1);
      bool listed = type(error).kind == TypeKind::Unit;
      for (auto e : n.errors)
        listed |= e == error;
      need(listed, "await task error is missing from the node error set");
      break;
    }
    case NodeKind::unsupported:
      fail("unsupported HIR node: " + std::get<Unsupported>(n.data).feature,
           n.source.span);
    }
  }
  static bool same_ref(const modules::NodeRef &a, const modules::NodeRef &b) {
    return a.module == b.module && a.file == b.file && a.node == b.node;
  }
  static bool same_span(const SourceSpan &a, const SourceSpan &b) {
    return a.snapshot == b.snapshot && a.begin_byte == b.begin_byte &&
           a.end_byte == b.end_byte;
  }
  static bool same_source(const Source &a, const Source &b) {
    return same_ref(a.node, b.node) && same_span(a.span, b.span);
  }
  void declaration(SymbolId id, const Source &src, modules::SymbolKind kind,
                   AstKind ast_kind) const {
    const auto &s = symbol(id);
    source(src);
    need(s.kind == kind && s.definition && same_ref(*s.definition, src.node),
         "declaration source/symbol kind mismatch");
    const auto &file = *m.resolved->project->modules[src.node.module.value]
                            .files[src.node.file];
    need(file.nodes[src.node.node.value].kind == ast_kind,
         "declaration AST kind mismatch");
  }
  TypeId source_type(modules::NodeRef r) const {
    const auto &file = *m.resolved->project->module(r.module).files.at(r.file);
    const auto &n = file.node(r.node);
    auto cr = [&](AstNodeId id) {
      auto c = r;
      c.node = id;
      return c;
    };
    if (n.kind == AstKind::ParenthesizedType ||
        n.kind == AstKind::Parenthesized ||
        n.kind == AstKind::BracketItem)
      return source_type(cr(n.children.at(0)));
    if (n.kind == AstKind::FunctionType)
      return source_signature(r);
    if (n.kind == AstKind::TraitObjectType) {
      need(n.children.size() == 1, "dyn type source arity");
      auto inner_ref = cr(n.children[0]);
      const auto &inner = file.node(inner_ref.node);
      need(inner.kind == AstKind::NamedType, "dyn type source is not a trait");
      std::optional<SymbolId> sid;
      for (const auto &o : m.resolved->occurrences)
        if (same_ref(o.source, inner_ref))
          sid = o.symbol;
      need(sid && symbol(*sid).kind == modules::SymbolKind::trait,
           "dyn source name is not a trait");
      const auto *tr = trait_declaration(*sid);
      need(tr != nullptr, "dyn source trait has no declaration");
      Type t;
      t.kind = TypeKind::TraitObject;
      t.declaration = *sid;
      for (auto child : inner.children) {
        auto arg = inner_ref;
        arg.node = child;
        t.type_args.push_back(source_type(arg));
      }
      need(t.type_args.size() == tr->params.size(),
           "dyn source trait argument count");
      return substituted.intern(std::move(t));
    }
    // A type-qualified method call writes its qualifier as an expression
    // Name/Member rather than a NamedType. Accept those source forms too so
    // the verifier can reconstruct the same TypeId without guessing from the
    // call's argument count or spelling.
    std::optional<SymbolId> sid;
    auto last_source_symbol = [&](modules::NodeRef ref) {
      ref = unwrapped(ref);
      std::optional<SymbolId> result;
      for (const auto &o : m.resolved->occurrences)
        if (same_ref(o.source, ref))
          result = o.symbol;
      return result;
    };
    if (n.kind == AstKind::NeutralBracket) {
      need(!n.children.empty(), "type qualifier has no base");
      sid = last_source_symbol(cr(n.children.front()));
    } else {
      need(n.kind == AstKind::NamedType || n.kind == AstKind::Name ||
               n.kind == AstKind::Member,
           "invalid type syntax source");
      sid = last_source_symbol(r);
    }
    need(sid.has_value(), "type syntax has no resolved symbol");
    const auto &s = symbol(*sid);
    Type t;
    if (s.kind == modules::SymbolKind::type_parameter) {
      need(n.kind != AstKind::NeutralBracket,
           "type parameter arguments differ from source");
      for (const auto &im : m.impls)
        if (im.self_parameter == *sid)
          return im.self_type;
      t.kind = TypeKind::TypeParameter;
      t.declaration = *sid;
    } else if (s.kind == modules::SymbolKind::type) {
      if (auto ex = m.resolved->foreign_export(*sid)) {
        // Name/Member syntax keeps its qualifier as a child; only a type
        // application supplies real arguments.  Foreign declarations never
        // take type arguments, matching sema's parse_type rule.
        const bool type_arguments =
            (n.kind == AstKind::NeutralBracket && n.children.size() > 1) ||
            (n.kind == AstKind::NamedType && !n.children.empty());
        need(ex->kind == modules::ForeignExportKind::type && !type_arguments,
             "foreign source type identity/arity");
        if (ex->c_type) {
          modules::CTypeRef ref;
          ref.kind = modules::CTypeRef::Kind::named;
          ref.named = *ex->c_type;
          return detail::c_type_id(*m.resolved->project, substituted, ref);
        }
        if (ex->c_type_ref)
          return detail::c_type_id(*m.resolved->project, substituted,
                                   *ex->c_type_ref);
        need(ex->declaration.has_value(), "foreign source type lacks declaration");
        Type ft; ft.kind = TypeKind::ForeignOpaque; ft.binding = *ex->declaration;
        return substituted.intern(ft);
      }
      auto it = nominals.find(sid->value);
      need(it != nominals.end(), "type syntax nominal declaration");
      t.kind = it->second->kind;
      t.declaration = *sid;
      if (n.kind == AstKind::NeutralBracket)
        for (size_t i = 1; i < n.children.size(); ++i)
          t.type_args.push_back(source_type(cr(n.children[i])));
      else
        for (auto c : n.children)
          if (n.kind == AstKind::NamedType)
            t.type_args.push_back(source_type(cr(c)));
    } else {
      need(s.kind == modules::SymbolKind::builtin_type,
           "type syntax symbol kind");
      if (private_runtime_type_name(s.name)) {
        const auto &owner = m.resolved->project->module(r.module).logical_path;
        need(s.name == "__woozy_IpcHandle" ? owner == "std/ipc"
                                          : private_runtime_type_module(owner),
             "private runtime type spelled outside its std module");
        const bool type_arguments =
            (n.kind == AstKind::NeutralBracket && n.children.size() > 1) ||
            (n.kind == AstKind::NamedType && !n.children.empty());
        need(!type_arguments, "private runtime type does not accept arguments");
        if (s.name == "__woozy_Thread")
          t.kind = TypeKind::ThreadHandle;
        else if (s.name == "__woozy_Mutex")
          t.kind = TypeKind::MutexHandle;
        else if (s.name == "__woozy_Condvar")
          t.kind = TypeKind::CondvarHandle;
        else if (s.name == "__woozy_IpcHandle")
          t.kind = TypeKind::IpcHandle;
        else
          t.kind = TypeKind::AtomicI64Handle;
      } else if (s.name == "Vec" || s.name == "Slice" ||
                 s.name == "Weak") {
        t.kind = s.name == "Vec" ? TypeKind::Vec
                : s.name == "Slice" ? TypeKind::Slice
                                    : TypeKind::Weak;
        if (n.kind == AstKind::NeutralBracket)
          for (size_t i = 1; i < n.children.size(); ++i)
            t.type_args.push_back(source_type(cr(n.children[i])));
        else
          for (auto c : n.children)
            if (n.kind == AstKind::NamedType)
              t.type_args.push_back(source_type(cr(c)));
        need(t.type_args.size() == 1, "container/Weak type arity");
      } else if (s.name == "Task") {
        t.kind = TypeKind::Task;
        if (n.kind == AstKind::NeutralBracket)
          for (size_t i = 1; i < n.children.size(); ++i)
            t.type_args.push_back(source_type(cr(n.children[i])));
        else
          for (auto c : n.children)
            if (n.kind == AstKind::NamedType)
              t.type_args.push_back(source_type(cr(c)));
        need(t.type_args.size() == 2, "Task result/error arity");
      } else if (s.name == "Ptr" || s.name == "PtrMut") {
        t.kind = TypeKind::CForeignPointer;
        t.bits = s.name == "PtrMut" ? 1 : 0;
        if (n.kind == AstKind::NeutralBracket)
          for (size_t i = 1; i < n.children.size(); ++i)
            t.type_args.push_back(source_type(cr(n.children[i])));
        else
          for (auto c : n.children)
            if (n.kind == AstKind::NamedType)
              t.type_args.push_back(source_type(cr(c)));
        need(t.type_args.size() == 1, "C pointer type arity");
      } else if (s.name == "Buffer" || s.name == "BufferMut") {
        t.kind = TypeKind::CForeignBuffer;
        t.bits = s.name == "BufferMut" ? 1 : 0;
        if (n.kind == AstKind::NeutralBracket)
          for (size_t i = 1; i < n.children.size(); ++i)
            t.type_args.push_back(source_type(cr(n.children[i])));
        else
          for (auto c : n.children)
            if (n.kind == AstKind::NamedType)
              t.type_args.push_back(source_type(cr(c)));
        need(t.type_args.size() == 1, "C buffer type arity");
      } else if (s.name == "Unit")
        t.kind = TypeKind::Unit;
      else if (s.name == "bool")
        t.kind = TypeKind::Bool;
      else if (s.name == "string")
        t.kind = TypeKind::String;
      else if (s.name == "isize")
        t.kind = TypeKind::Isize;
      else if (s.name == "usize")
        t.kind = TypeKind::Usize;
      else if (s.name == "f32" || s.name == "f64") {
        t.kind = TypeKind::Float;
        t.bits = s.name == "f32" ? 32 : 64;
      } else {
        need(s.name.size() >= 2 && (s.name[0] == 'i' || s.name[0] == 'u'),
             "unimplemented type syntax");
        t.kind = s.name[0] == 'i' ? TypeKind::SignedInt : TypeKind::UnsignedInt;
        t.bits = s.name == "i8" || s.name == "u8"     ? 8
                 : s.name == "i16" || s.name == "u16" ? 16
                 : s.name == "i32" || s.name == "u32" ? 32
                                                      : 64;
      }
    }
    return substituted.intern(t);
  }
  TypeId source_signature(modules::NodeRef r) const {
    const auto &file = *m.resolved->project->module(r.module).files.at(r.file);
    Type t;
    t.kind = TypeKind::Function;
    Type u;
    u.kind = TypeKind::Unit;
    const TypeId unit_id = substituted.intern(u);
    t.result = unit_id;
    const auto parts = split_signature(file, r.node);
    for (auto c : parts.parameters) {
      auto cr = r;
      const auto &n = file.node(c);
      auto mode = ParamMode::ordinary;
      for (size_t i = n.token_begin;
           i < file.node(n.children.at(0)).token_begin; ++i) {
        if (file.tokens.at(i).spelling == "inout")
          mode = ParamMode::inout;
        if (file.tokens.at(i).spelling == "move")
          mode = ParamMode::move;
      }
      cr.node = n.children.at(0);
      t.parameters.push_back({mode, source_type(cr)});
    }
    if (parts.result) {
      auto cr = r;
      cr.node = *parts.result;
      t.result = source_type(cr);
    }
    if (parts.throws_type) {
      auto cr = r;
      cr.node = *parts.throws_type;
      t.throws_type = source_type(cr);
    }
    bool async = false;
    for (auto c : file.node(r.node).children)
      async |= file.node(c).kind == AstKind::Async;
    if (async) {
      // The published source signature of an async declaration returns
      // Task[R,E]; the declared throws clause moves into the Task.
      Type task;
      task.kind = TypeKind::Task;
      task.type_args = {t.result, t.throws_type.value_or(unit_id)};
      Type callable = t;
      callable.result = substituted.intern(std::move(task));
      callable.throws_type.reset();
      t = callable;
    }
    return substituted.intern(t);
  }
  std::vector<SymbolId> source_generics(const Source &f) const {
    const auto &file =
        *m.resolved->project->module(f.node.module).files.at(f.node.file);
    std::vector<std::pair<size_t, SymbolId>> params;
    for (const auto &owner : file.nodes) {
      if (owner.kind != AstKind::Function &&
          owner.kind != AstKind::FunctionLiteral &&
          owner.kind != AstKind::Impl && owner.kind != AstKind::Struct &&
          owner.kind != AstKind::Enum)
        continue;
      if (owner.span.begin_byte > f.span.begin_byte ||
          owner.span.end_byte < f.span.end_byte)
        continue;
      for (auto id : owner.children)
        if (file.node(id).kind == AstKind::GenericParameter) {
          auto ref = f.node;
          ref.node = id;
          std::optional<SymbolId> sid;
          for (const auto &s : m.resolved->symbols)
            if (s.kind == modules::SymbolKind::type_parameter && s.definition &&
                same_ref(*s.definition, ref))
              sid = s.id;
          need(sid.has_value(), "generic parameter source has no symbol");
          params.push_back({file.node(id).span.begin_byte, *sid});
        }
    }
    std::sort(params.begin(), params.end(),
              [](auto a, auto b) { return a.first < b.first; });
    std::vector<SymbolId> result;
    for (auto [offset, id] : params)
      result.push_back(id);
    return result;
  }

  // Read the ordered generic parameters belonging to a source declaration,
  // including the real SymbolId, its single declared bound and the parameter
  // Source. This is used for trait prototypes whose public TraitDecl has no
  // parameter field, and for the own suffix of an impl method.
  std::vector<GenericParam> source_parameters(const Source &f) const {
    std::vector<GenericParam> result;
    const auto &file =
        *m.resolved->project->module(f.node.module).files.at(f.node.file);
    for (auto sid : source_generics(f)) {
      const auto &s = symbol(sid);
      need(s.definition && s.declaration,
           "source generic parameter lacks a real declaration");
      Source src{*s.definition, *s.declaration};
      std::optional<TraitRef> bound;
      const auto &syntax = file.node(s.definition->node);
      if (!syntax.children.empty()) {
        auto ref = *s.definition;
        ref.node = syntax.children.front();
        bound = source_trait_application(ref);
      }
      result.push_back({sid, bound, src});
    }
    return result;
  }
  bool matches(TypeId pattern, TypeId actual, Substitutions &sub) const {
    // Substituted bound arguments may live in the verifier's derived context,
    // which is a superset of m.types. Resolve there first; producer TypeIds
    // keep the same definitions.
    auto resolve = [&](TypeId id) -> const Type & {
      if (auto t = substituted.find(id))
        return *t;
      return type(id);
    };
    const auto &p = resolve(pattern), &a = resolve(actual);
    if (p.kind == TypeKind::TypeParameter) {
      for (auto [s, t] : sub)
        if (s == p.declaration)
          return t == actual;
      sub.push_back({p.declaration, actual});
      return true;
    }
    if (p.kind != a.kind || p.bits != a.bits ||
        p.declaration != a.declaration ||
        p.type_args.size() != a.type_args.size() ||
        p.parameters.size() != a.parameters.size() ||
        p.throws_type.has_value() != a.throws_type.has_value())
      return false;
    for (size_t i = 0; i < p.type_args.size(); ++i)
      if (!matches(p.type_args[i], a.type_args[i], sub))
        return false;
    if (p.kind == TypeKind::Function) {
      for (size_t i = 0; i < p.parameters.size(); ++i)
        if (p.parameters[i].mode != a.parameters[i].mode ||
            !matches(p.parameters[i].type, a.parameters[i].type, sub))
          return false;
      if (!matches(p.result, a.result, sub))
        return false;
      if (p.throws_type && !matches(*p.throws_type, *a.throws_type, sub))
        return false;
    }
    return true;
  }
  bool satisfies(TypeId actual, const TraitRef &trait,
                 size_t depth = 0) const {
    std::vector<uint64_t> args;
    for (auto a : trait.args)
      args.push_back(a.value);
    auto key = std::make_tuple(actual.value, trait.declaration.value,
                               std::move(args));
    if (auto it = trait_results.find(key); it != trait_results.end())
      return it->second;
    if (depth >= 64 || !active_constraints.insert(key).second)
      return false;
    const Type &a = substituted.find(actual) ? *substituted.find(actual)
                                             : type(actual);
    size_t count = 0;
    if (a.kind == TypeKind::TypeParameter) {
      auto contains = [&](const auto &ps) {
        for (auto p : ps)
          if (p.symbol == a.declaration && p.trait && *p.trait == trait)
            return true;
        return false;
      };
      for (const auto &t : m.templates)
        if (contains(t.params))
          count = 1;
      for (const auto &d : m.nominals)
        if (contains(d.params))
          count = 1;
      for (const auto &im : m.impls)
        if (contains(im.params))
          count = 1;
      for (const auto &tr : m.traits) {
        if (contains(tr.params))
          count = 1;
        if (tr.self == a.declaration &&
            trait_self_application(tr) == trait)
          count = 1;
      }
    } else
      for (const auto &im : m.impls) {
        if (!im.trait || im.trait->declaration != trait.declaration ||
            im.trait->args.size() != trait.args.size())
          continue;
        Substitutions sub;
        if (!matches(im.self_type, actual, sub))
          continue;
        bool matched_args = true;
        for (size_t i = 0; i < trait.args.size(); ++i)
          if (!matches(im.trait->args[i], trait.args[i], sub)) {
            matched_args = false;
            break;
          }
        if (!matched_args)
          continue;
        bool valid = true;
        for (auto p : im.params) {
          auto it = std::find_if(sub.begin(), sub.end(),
                                 [&](auto x) { return x.first == p.symbol; });
          if (it == sub.end()) {
            valid = false;
            break;
          }
          if (p.trait) {
            TraitRef bound = *p.trait;
            for (auto &arg : bound.args)
              arg = substitute(arg, sub, substituted);
            if (!satisfies(it->second, bound, depth + 1)) {
              valid = false;
              break;
            }
          }
        }
        if (valid)
          ++count;
      }
    active_constraints.erase(key);
    return trait_results[key] = count == 1;
  }
  void constraints(const std::vector<GenericParam> &ps,
                   const std::vector<TypeId> &args,
                   const Substitutions *outer = nullptr) const {
    need(ps.size() == args.size(), "constraint type argument count");
    Substitutions sub = outer ? *outer : Substitutions{};
    auto bind = [&](SymbolId symbol, TypeId value) {
      auto it = std::find_if(sub.begin(), sub.end(), [&](auto entry) {
        return entry.first == symbol;
      });
      if (it == sub.end())
        sub.push_back({symbol, value});
      else
        it->second = value;
    };
    for (size_t i = 0; i < ps.size(); ++i)
      bind(ps[i].symbol, args[i]);
    for (size_t i = 0; i < ps.size(); ++i) {
      if (!ps[i].trait)
        continue;
      TraitRef bound = *ps[i].trait;
      const auto *tr = trait_declaration(bound.declaration);
      need(symbol(bound.declaration).kind == modules::SymbolKind::trait && tr &&
               tr->params.size() == bound.args.size(),
           "declared constraint application identity/arity");
      for (auto &arg : bound.args)
        arg = substitute(arg, sub, substituted);
      check_trait_parameter_bounds(bound, args[i]);
      need(satisfies(args[i], bound),
           "type arguments do not satisfy declared constraints");
    }
  }
  void finite_layouts() const {
    need(!infinite_value_layout(m, stop), "infinite recursive value layout");
  }

  // outer_allowed supplies only the real surrounding type environment, such
  // as a trait prototype's Self. It is never a blanket permission for any
  // TypeParameter: each caller passes its own actual allowed identity set.
  std::set<uint64_t> parameter_set(const std::vector<GenericParam> &ps,
                                   const std::set<uint64_t> &outer_allowed =
                                       {}) const {
    std::set<uint64_t> result;
    for (const auto &p : ps) {
      declaration(p.symbol, p.source, modules::SymbolKind::type_parameter,
                  AstKind::GenericParameter);
      need(result.insert(p.symbol.value).second, "duplicate generic parameter");
      const auto &file = *m.resolved->project->module(p.source.node.module)
                              .files.at(p.source.node.file);
      const auto &syntax = file.node(p.source.node.node);
      std::optional<TraitRef> declared;
      if (!syntax.children.empty()) {
        auto ref = p.source.node;
        ref.node = syntax.children.at(0);
        declared = source_trait_application(ref);
      }
      need(declared == p.trait,
           "template constraint differs from source declaration");
    }
    std::set<uint64_t> allowed = result;
    allowed.insert(outer_allowed.begin(), outer_allowed.end());
    for (const auto &p : ps)
      if (p.trait) {
        check_trait_application(*p.trait, allowed);
        check_trait_parameter_bounds(*p.trait, parameter_type(p.symbol));
      }
    return result;
  }
  void self_owner(const Source &owner, SymbolId self) const {
    need(symbol(self).kind == modules::SymbolKind::type_parameter,
         "Self is not a type parameter");
    size_t found = 0;
    for (auto [ref, s] : m.resolved->self_parameters)
      if (s == self && same_ref(ref, owner.node))
        ++found;
    need(found == 1, "Self has no unique real trait/impl owner");
  }
  template <class Entries>
  void source_entries(const Source &owner, AstKind kind,
                      const Entries &entries) const {
    const auto &file = *m.resolved->project->module(owner.node.module)
                            .files.at(owner.node.file);
    std::vector<AstNodeId> expected;
    for (auto id : file.node(owner.node.node).children)
      if (file.node(id).kind == kind)
        expected.push_back(id);
    need(expected.size() == entries.size(),
         "declaration member count differs from source");
    for (size_t i = 0; i < expected.size(); ++i) {
      auto ref = owner.node;
      ref.node = expected[i];
      need(same_ref(entries[i].source.node, ref),
           "declaration member owner/order differs from source");
    }
  }
  // Self and every ordered trait argument must admit one common substitution.
  // Intern a synthetic function type in the verifier's own context so the
  // shared unifier solves all columns with one binding table.
  bool impl_patterns_overlap(const ImplDecl &a, const ImplDecl &b) const {
    if (!a.trait || !b.trait)
      return type_patterns_overlap(a.self_type, b.self_type, m.types);
    if (a.trait->args.size() != b.trait->args.size())
      return false;
    Type unit_type;
    unit_type.kind = TypeKind::Unit;
    Type fa;
    fa.kind = TypeKind::Function;
    fa.result = substituted.intern(unit_type);
    fa.parameters.push_back({ParamMode::ordinary, a.self_type});
    for (auto t : a.trait->args)
      fa.parameters.push_back({ParamMode::ordinary, t});
    Type fb = fa;
    fb.parameters[0].type = b.self_type;
    for (size_t i = 0; i < b.trait->args.size(); ++i)
      fb.parameters[i + 1].type = b.trait->args[i];
    return type_patterns_overlap(substituted.intern(std::move(fa)),
                                 substituted.intern(std::move(fb)),
                                 substituted);
  }
  void declarations() {
    for (const auto &d : m.nominals) {
      need(nominals.emplace(d.symbol.value, &d).second,
           "duplicate nominal declaration");
      need(d.kind == TypeKind::NominalStruct || d.kind == TypeKind::NominalEnum,
           "nominal declaration kind");
      declaration(d.symbol, d.source, modules::SymbolKind::type,
                  d.kind == TypeKind::NominalStruct ? AstKind::Struct
                                                    : AstKind::Enum);
    }
    for (const auto &d : m.nominals) {
      source_entries(d.source, AstKind::GenericParameter, d.params);
      source_entries(d.source, AstKind::Field, d.fields);
      source_entries(d.source, AstKind::Variant, d.variants);
      auto ps = parameter_set(d.params);
      std::set<uint64_t> fields, variants;
      auto field = [&](const FieldDecl &f) {
        declaration(f.symbol, f.source, modules::SymbolKind::field,
                    AstKind::Field);
        need(fields.insert(f.symbol.value).second, "duplicate field identity");
        need(f.source.node.module == d.source.node.module &&
                 f.source.node.file == d.source.node.file &&
                 f.source.span.begin_byte >= d.source.span.begin_byte &&
                 f.source.span.end_byte <= d.source.span.end_byte,
             "field belongs to another declaration");
        valid_type(f.type, ps);
        auto ref = f.source.node;
        ref.node = m.resolved->project->module(ref.module)
                       .files.at(ref.file)
                       ->node(ref.node)
                       .children.at(0);
        need(f.type == source_type(ref),
             "field type differs from source declaration");
      };
      need(d.kind == TypeKind::NominalStruct ? d.variants.empty()
                                             : d.fields.empty(),
           "struct/enum declaration payload");
      for (const auto &f : d.fields)
        field(f);
      for (const auto &v : d.variants) {
        declaration(v.symbol, v.source, modules::SymbolKind::variant,
                    AstKind::Variant);
        need(variants.insert(v.symbol.value).second,
             "duplicate variant identity");
        source_entries(v.source, AstKind::Field, v.payload);
        for (const auto &f : v.payload)
          field(f);
      }
    }
    std::set<uint64_t> traits;
    for (const auto &tr : m.traits) {
      declaration(tr.symbol, tr.source, modules::SymbolKind::trait,
                  AstKind::Trait);
      need(traits.insert(tr.symbol.value).second, "duplicate trait identity");
      self_owner(tr.source, tr.self);
      source_entries(tr.source, AstKind::GenericParameter, tr.params);
      parameter_set(tr.params, {tr.self.value});
      check_trait_parameter_bounds(trait_self_application(tr),
                                   parameter_type(tr.self));
      std::set<uint64_t> methods;
      for (const auto &method : tr.methods) {
        declaration(method.symbol, method.source, modules::SymbolKind::function,
                    AstKind::Function);
        need(methods.insert(method.symbol.value).second,
             "duplicate trait method");
        auto own = source_parameters(method.source);
        // The prototype's own bounds may refer to the real trait Self and any
        // ordered trait parameter. Put those exact identities into the bound
        // environment before validating the own list; none of them joins the
        // method-own parameters or the instance key.
        std::set<uint64_t> trait_environment{tr.self.value};
        for (const auto &p : tr.params)
          trait_environment.insert(p.symbol.value);
        auto ps = parameter_set(own, trait_environment);
        ps.insert(trait_environment.begin(), trait_environment.end());
        valid_type(method.signature, ps);
        need(method.signature == source_signature(method.source.node),
             "trait signature differs from source prototype");
        const auto &sig = type(method.signature);
        need(sig.kind == TypeKind::Function &&
                 sig.parameters.size() == method.parameters.size(),
             "trait prototype signature");
        if (!own.empty() || !tr.params.empty()) {
          // Top-level move and inout, including the real first-position trait
          // Self, are source modes. The actual self is the raw trait Self,
          // not an own type parameter merely labelled self. Concrete
          // applications are rechecked by the shared inout data rule; nested
          // callback data is not rejected for dependency alone.
          for (const auto &p : sig.parameters)
            need(p.mode == ParamMode::ordinary ||
                     p.mode == ParamMode::move ||
                     p.mode == ParamMode::inout,
                 "generic trait method move/inout is unsupported");
        }
        for (const auto &f : m.functions)
          need(f.symbol != method.symbol,
               "trait prototype has fake executable body");
        for (const auto &t : m.templates)
          need(t.function.symbol != method.symbol,
               "trait prototype has fake template body");
      }
    }
    for (size_t i = 0; i < m.impls.size(); ++i)
      for (size_t j = 0; j < i; ++j) {
        const auto &a = m.impls[i], &b = m.impls[j];
        const bool same_target = (!a.trait && !b.trait) ||
                                 (a.trait && b.trait &&
                                  a.trait->declaration == b.trait->declaration);
        if (!same_target)
          continue;
        need(!impl_patterns_overlap(a, b),
             "duplicate or overlapping impl patterns");
      }
    for (const auto &im : m.impls) {
      source(im.source);
      self_owner(im.source, im.self_parameter);
      // An impl parameter bound may refer to the impl's real Self identity.
      auto ps = parameter_set(im.params, {im.self_parameter.value});
      valid_type(im.self_type, ps);
      need(type(im.self_type).kind == TypeKind::NominalStruct ||
               type(im.self_type).kind == TypeKind::NominalEnum ||
               number(type(im.self_type).kind) ||
               type(im.self_type).kind == TypeKind::Bool ||
               type(im.self_type).kind == TypeKind::Vec ||
               type(im.self_type).kind == TypeKind::Slice ||
               type(im.self_type).kind == TypeKind::String ||
               type(im.self_type).kind == TypeKind::Unit,
           "impl self type is not an implemented value type");
      // Rebuild the impl header from the real Source: a trait application
      // must match the actual declaration and the ordered written arguments,
      // and the Self type must still be the one declared after `for`.
      const auto &file = *m.resolved->project->module(im.source.node.module)
                              .files.at(im.source.node.file);
      const auto &syntax = file.node(im.source.node.node);
      std::vector<modules::NodeRef> types;
      for (auto id : syntax.children)
        if (file.node(id).kind == AstKind::NamedType) {
          auto ref = im.source.node;
          ref.node = id;
          types.push_back(ref);
        }
      need(types.size() == (im.trait ? 2u : 1u),
           "impl source declaration arity");
      need(source_type(types.back()) == im.self_type,
           "impl self type differs from source");
      if (im.trait) {
        need(traits.count(im.trait->declaration.value), "impl trait missing");
        need(source_trait_application(types.front()) == *im.trait,
             "impl trait application differs from source declaration");
        check_trait_application(*im.trait, ps);
        check_trait_parameter_bounds(*im.trait, im.self_type);
      }
    }
    // Complete trait applications are checked separately after substituting
    // Self and every ordered trait argument. There is no scalar-only policy
    // for Self-dependent callback data.
  }
  TypeId source_type_in_function(modules::NodeRef ref, SymbolId owner) const {
    auto t = source_type(ref);
    for (const auto &i : m.instances)
      if (i.symbol == owner) {
        const auto &ps = templates.at(i.origin.value)->params;
        need(ps.size() == i.type_args.size(), "instance error type arity");
        Substitutions sub;
        for (size_t p = 0; p < ps.size(); ++p)
          sub.emplace_back(ps[p].symbol, i.type_args[p]);
        return substitute(t, sub, substituted);
      }
    return t;
  }
  void effect_shape(const HirNode &n) const {
    unit(n);
    const auto &file = *m.resolved->project->module(n.source.node.module)
                            .files.at(n.source.node.file);
    const auto &ast = file.node(n.source.node.node);
    auto source_child = [&](HirNodeId id, AstNodeId expected) {
      auto ref = n.source.node;
      ref.node = expected;
      need(same_ref(node(id).source.node, ref),
           "effect body differs from source");
    };
    if (auto t = std::get_if<Try>(&n.data)) {
      need(ast.kind == AstKind::Try &&
               ast.children.size() == t->catches.size() + 1 &&
               n.children.size() == ast.children.size() && !t->catches.empty(),
           "try source/handler count");
      need(node(n.children[0]).kind() == NodeKind::block,
           "try protected block");
      source_child(n.children[0], ast.children[0]);
      std::set<uint64_t> caught;
      for (size_t i = 0; i < t->catches.size(); ++i) {
        const auto &c = t->catches[i];
        source(c.source);
        auto ref = n.source.node;
        ref.node = ast.children[i + 1];
        need(same_ref(c.source.node, ref), "catch source/order mismatch");
        declaration(c.binding, c.source, modules::SymbolKind::value,
                    AstKind::Catch);
        const auto &ca = file.node(ref.node);
        need(ca.children.size() == 2, "catch syntax shape");
        ref.node = ca.children[0];
        valid_type(c.error_type, allowed(scope(n.scope).function));
        need(c.error_type ==
                 source_type_in_function(ref, scope(n.scope).function),
             "catch error type differs from source");
        need(caught.insert(c.error_type.value).second, "duplicate catch type");
        need(c.body == n.children[i + 1] &&
                 node(c.body).kind() == NodeKind::block &&
                 node(c.body).scope == c.scope &&
                 scope(c.scope).parent == n.scope &&
                 same_ref(scope(c.scope).source.node, c.source.node),
             "catch scope/body mismatch");
        source_child(c.body, ca.children[1]);
      }
    } else if (auto d = std::get_if<Defer>(&n.data)) {
      need(ast.kind == AstKind::Defer && ast.children.size() == 1 &&
               n.children.size() == 1 &&
               node(n.children[0]).kind() == NodeKind::block &&
               d->owner_scope == n.scope,
           "defer source/body/registration scope");
      source_child(n.children[0], ast.children[0]);
      for (auto r : d->references)
        symbol(r);
    } else {
      need(ast.kind == AstKind::Throw && ast.children.size() == 1 &&
               n.children.size() == 1,
           "throw source/arity");
      const auto &value = expr(n.children[0]);
      const auto &original = file.node(ast.children[0]);
      need(value.source.span.begin_byte >= original.span.begin_byte &&
               value.source.span.end_byte <= original.span.end_byte,
           "throw expression differs from source");
    }
  }

  void register_pattern(const Pattern &p, ScopeId s, SymbolId f, size_t ready) {
    source(p.source);
    valid_type(p.type, allowed(f));
    need(!(p.variant && p.binding) && !(p.variant && p.literal) &&
             !(p.binding && p.literal),
         "pattern variant/binding/literal are mutually exclusive");
    if (p.variant) {
      need(!p.binding && !p.literal, "variant pattern fields");
    } else if (p.binding) {
      need(!p.literal && p.payload.empty(), "binding pattern payload");
      declaration(*p.binding, p.source, modules::SymbolKind::value,
                  AstKind::BindPattern);
      need(locals.emplace(p.binding->value, Local{p.type, s, f, false, ready})
               .second,
           "duplicate pattern binding identity");
    } else if (p.literal) {
      need(p.payload.empty(), "literal pattern payload");
    } else {
      need(p.payload.empty(), "wildcard pattern payload");
    }
    for (const auto &c : p.payload)
      register_pattern(c, s, f, ready);
  }
  const AstNode &pattern_ast(const Source &s) const {
    return m.resolved->project->module(s.node.module)
        .files.at(s.node.file)
        ->node(s.node.node);
  }
  std::string_view pattern_variant_name(const Source &s) const {
    const auto &file =
        *m.resolved->project->module(s.node.module).files.at(s.node.file);
    const auto &ast = file.node(s.node.node);
    std::string_view name;
    unsigned depth = 0;
    for (size_t i = ast.token_begin; i < ast.token_end; ++i) {
      const auto token = file.tokens[i].spelling;
      if (token == "(" && !depth)
        break;
      if (token == "[")
        ++depth;
      if (token == "]")
        --depth;
      if (token == "." && !depth)
        name = file.tokens[i + 1].spelling;
    }
    return name;
  }
  void pattern_shape(const Pattern &p, TypeId expected, SymbolId owner,
                     std::optional<Source> parent = std::nullopt,
                     std::optional<Source> required = std::nullopt) const {
    source(p.source);
    need(p.type == expected, "pattern type mismatch");
    if (required)
      need(same_source(p.source, *required),
           "pattern source is not the exact AST pattern");
    if (parent)
      need(p.source.node.module == parent->node.module &&
               p.source.node.file == parent->node.file &&
               p.source.span.snapshot == parent->span.snapshot &&
               p.source.span.begin_byte >= parent->span.begin_byte &&
               p.source.span.end_byte <= parent->span.end_byte,
           "nested pattern source belongs to another pattern");
    const auto &ast = pattern_ast(p.source);
    const auto &file =
        *m.resolved->project->module(p.source.node.module)
             .files.at(p.source.node.file);
    need(!(p.variant && p.binding) && !(p.variant && p.literal) &&
             !(p.binding && p.literal),
         "pattern variant/binding/literal are mutually exclusive");
    if (!p.variant && !p.literal) {
      need(ast.kind == AstKind::BindPattern || ast.kind == AstKind::Case,
           "wildcard/binding pattern source kind");
      if (p.binding) {
        need(ast.kind == AstKind::BindPattern &&
                 file.tokens.at(ast.token_begin).spelling != "_",
             "binding pattern source/name");
        need(symbol(*p.binding).name ==
                 file.tokens.at(ast.token_begin).spelling,
             "binding pattern symbol differs from source");
      } else if (ast.kind == AstKind::BindPattern)
        need(file.tokens.at(ast.token_begin).spelling == "_",
             "wildcard pattern source/name");
      else
        need(ast.token_begin < ast.token_end &&
                 file.tokens.at(ast.token_begin).spelling == "default",
             "wildcard pattern source is not an explicit default");
      need(p.payload.empty(), "wildcard/binding payload");
      return;
    }
    if (p.literal) {
      need(!p.binding && !p.variant && p.payload.empty(),
           "literal pattern payload/binding");
      need(ast.kind == AstKind::LiteralPattern, "literal pattern source kind");
      PatternLiteral decoded;
      try {
        decoded = decode_pattern_literal(*m.resolved->project, substituted,
                                         p.source.node, expected, m.config);
      } catch (const LiteralError &e) {
        fail(e.what(), p.source.span);
      }
      need(decoded.index() == p.literal->index(),
           "literal pattern alternative differs from its type");
      std::visit(
          [&](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            const auto *written = std::get_if<T>(&*p.literal);
            need(written != nullptr, "literal pattern alternative mismatch");
            if constexpr (std::is_same_v<T, BoolLiteral>)
              need(written->value == value.value,
                   "bool literal pattern value differs from source");
            else if constexpr (std::is_same_v<T, IntegerLiteral>)
              need(written->decimal == value.decimal &&
                       written->bits == value.bits,
                   "integer literal pattern value differs from source");
            else if constexpr (std::is_same_v<T, FloatLiteral>)
              need(written->bits == value.bits,
                   "float literal pattern bits differ from source");
            else
              need(written->utf8 == value.utf8,
                   "string literal pattern bytes differ from source");
          },
          decoded);
      return;
    }
    need(ast.kind == AstKind::VariantPattern, "variant pattern source kind");
    need(!p.binding && !p.literal &&
             type(expected).kind == TypeKind::NominalEnum,
         "variant pattern type");
    std::optional<SymbolId> base;
    for (const auto &occurrence : m.resolved->occurrences)
      if (same_ref(occurrence.source, p.source.node))
        base = occurrence.symbol;
    need(base && *base == type(expected).declaration,
         "pattern source enum identity");
    const VariantDecl *variant = nullptr;
    for (const auto &v : nominal_type(expected).variants)
      if (v.symbol == *p.variant)
        variant = &v;
    need(variant && p.payload.size() == variant->payload.size(),
         "pattern variant/payload mismatch");
    const auto name = pattern_variant_name(p.source);
    need(!name.empty() && symbol(*p.variant).name == name,
         "pattern variant name differs from source");
    size_t type_argument = 0;
    size_t payload_index = 0;
    uint64_t previous_end = p.source.span.begin_byte;
    for (auto child : ast.children) {
      const auto kind = file.node(child).kind;
      if (kind == AstKind::NamedType || kind == AstKind::FunctionType ||
          kind == AstKind::ParenthesizedType) {
        need(type_argument < type(p.type).type_args.size(),
             "pattern type argument count");
        auto ref = p.source.node;
        ref.node = child;
        need(source_type_in_function(ref, owner) ==
                 type(p.type).type_args[type_argument++],
             "pattern type argument differs from source");
        continue;
      }
      need(payload_index < p.payload.size(),
           "wrong variant pattern payload count");
      const auto &child_pattern = p.payload[payload_index];
      const auto &child_ast = file.node(child);
      need(child_ast.span.snapshot == p.source.span.snapshot &&
               child_ast.span.begin_byte >= previous_end &&
               child_ast.span.end_byte <= p.source.span.end_byte,
           "pattern payload source/order");
      previous_end = child_ast.span.end_byte;
      Source required_child{p.source.node, child_ast.span};
      required_child.node.node = child;
      pattern_shape(child_pattern,
                    field_type(expected, variant->payload[payload_index]),
                    owner, p.source, required_child);
      ++payload_index;
    }
    need(payload_index == p.payload.size(),
         "wrong variant pattern payload count");
  }

  void aggregate_shape(const HirNode &n) const {
    if (n.kind() == NodeKind::field_read || n.kind() == NodeKind::field_place) {
      need(n.children.size() == 1, "field receiver count");
      const auto &base = node(n.children[0]);
      need(type(base.type).kind == TypeKind::NominalStruct,
           "field receiver nominal kind");
      auto field = n.kind() == NodeKind::field_read
                       ? std::get<FieldRead>(n.data).field
                       : std::get<FieldPlace>(n.data).field;
      const FieldDecl *decl = nullptr;
      for (const auto &f : nominal_type(base.type).fields)
        if (f.symbol == field)
          decl = &f;
      need(decl && n.type == field_type(base.type, *decl),
           "field identity/type mismatch");
      const auto &file = *m.resolved->project->module(n.source.node.module)
                              .files.at(n.source.node.file);
      const auto &syntax = file.node(n.source.node.node);
      need(syntax.kind == AstKind::Member && !syntax.children.empty() &&
               syntax.token_end > syntax.token_begin &&
               syntax.token_end <= file.tokens.size(),
           "field source is not a real Member");
      need(symbol(field).name == file.tokens.at(syntax.token_end - 1).spelling,
           "field identity differs from the source member name");
      auto receiver_source = n.source.node;
      receiver_source.node = syntax.children[0];
      need(same_ref(unwrapped(node(n.children[0]).source.node),
                    unwrapped(receiver_source)),
           "field receiver differs from the real Member base");
      if (n.kind() == NodeKind::field_read)
        expr(n.children[0]);
      else
        need(base.kind() == NodeKind::place ||
                 base.kind() == NodeKind::field_place ||
                 base.kind() == NodeKind::index_place,
             "field write requires writable base");
      return;
    }
    if (n.kind() == NodeKind::struct_init || n.kind() == NodeKind::enum_init) {
      auto kind = n.kind() == NodeKind::struct_init ? TypeKind::NominalStruct
                                                    : TypeKind::NominalEnum;
      need(type(n.type).kind == kind, "constructor nominal kind");
      const auto &d = nominal_type(n.type);
      const std::vector<FieldDecl> *fields = &d.fields;
      const std::vector<CallArg> *args = nullptr;
      if (auto c = std::get_if<StructInit>(&n.data))
        args = &c->fields;
      else {
        const auto &en = std::get<EnumInit>(n.data);
        args = &en.payload;
        fields = nullptr;
        for (const auto &v : d.variants)
          if (v.symbol == en.variant)
            fields = &v.payload;
      }
      need(fields && args->size() == fields->size() &&
               n.children.size() == args->size(),
           "constructor field/variant arity");
      const auto &file = *m.resolved->project->module(n.source.node.module)
                              .files.at(n.source.node.file);
      const auto &syntax = file.node(n.source.node.node);
      need((syntax.kind == AstKind::Call &&
            syntax.children.size() == args->size() + 1) ||
               (kind == TypeKind::NominalEnum && args->empty() &&
                syntax.kind == AstKind::Member),
           "constructor source kind/arity");
      if (kind == TypeKind::NominalEnum) {
        const auto &callee = syntax.kind == AstKind::Call
                                 ? file.node(syntax.children[0])
                                 : syntax;
        need(callee.kind == AstKind::Member && callee.token_end > 0 &&
                 callee.token_end <= file.tokens.size() &&
                 file.tokens[callee.token_end - 1].spelling ==
                     symbol(std::get<EnumInit>(n.data).variant).name,
             "constructor source variant identity");
      }
      std::set<uint32_t> seen;
      bool named = false;
      for (size_t i = 0; i < args->size(); ++i) {
        const auto &a = (*args)[i];
        need(a.source_index == i && a.parameter_index < fields->size() &&
                 seen.insert(a.parameter_index).second,
             "constructor argument mapping");
        const auto &f = (*fields)[a.parameter_index];
        need(n.children[i] == a.expression && a.mode == ParamMode::ordinary &&
                 expr(a.expression).type == field_type(n.type, f),
             "constructor argument type/mode/order");
        const auto &argument = file.node(syntax.children[i + 1]);
        need(argument.kind == AstKind::Argument &&
                 argument.children.size() == 1 &&
                 argument.token_begin + 1 < file.tokens.size(),
             "constructor source argument");
        bool labelled = file.tokens[argument.token_begin + 1].spelling == ":";
        need(a.label.has_value() == labelled,
             "constructor source label presence");
        if (a.label) {
          named = true;
          need(*a.label == symbol(f.symbol).name &&
                   *a.label == file.tokens[argument.token_begin].spelling,
               "constructor field/source label mismatch");
        } else
          need(kind == TypeKind::NominalEnum && !named &&
                   a.parameter_index == i,
               "constructor positional argument index/order");
        auto er = argument.children[0];
        const auto &original = file.node(er);
        // Parentheses and unary + may disappear during checking. The value
        // must still come from this source argument, with no erased mode.
        for (;;) {
          const auto &value = file.node(er);
          if (value.kind == AstKind::Parenthesized ||
              (value.kind == AstKind::Unary &&
               file.tokens[value.token_begin].spelling == "+")) {
            need(value.children.size() == 1, "constructor source wrapper");
            er = value.children[0];
            continue;
          }
          need(value.kind != AstKind::Unary ||
                   (file.tokens[value.token_begin].spelling != "inout" &&
                    file.tokens[value.token_begin].spelling != "move"),
               "constructor source argument mode");
          break;
        }
        const auto &src = node(a.expression).source;
        need(src.node.module == n.source.node.module &&
                 src.node.file == n.source.node.file &&
                 src.span.begin_byte >= original.span.begin_byte &&
                 src.span.end_byte <= original.span.end_byte,
             "constructor expression differs from source argument");
      }
      return;
    }
    if (auto s = std::get_if<Switch>(&n.data)) {
      unit(n);
      need(n.children.size() == s->arms.size() + 1 && !n.children.empty(),
           "switch child count");
      auto scrutinee = expr(n.children[0]).type;
      PatternCoverage coverage(
          scrutinee,
          [&](TypeId t) { return pattern_domain(m, t, substituted); },
          stop);
      for (size_t i = 0; i < s->arms.size(); ++i) {
        const auto &a = s->arms[i];
        source(a.source);
        const auto &arm_file =
            *m.resolved->project->module(a.source.node.module)
                 .files.at(a.source.node.file);
        const auto &case_ast = arm_file.node(a.source.node.node);
        need(case_ast.kind == AstKind::Case,
             "switch arm source is not a case");
        std::optional<Source> required;
        if (case_ast.token_begin < case_ast.token_end &&
            arm_file.tokens.at(case_ast.token_begin).spelling == "default") {
          required = a.source;
        } else {
          need(!case_ast.children.empty(), "case pattern child missing");
          auto child = case_ast.children[0];
          auto child_ref = a.source.node;
          child_ref.node = child;
          required = Source{child_ref, arm_file.node(child).span};
        }
        pattern_shape(a.pattern, scrutinee, scope(n.scope).function,
                      a.source, required);
        need(same_ref(scope(a.scope).source.node, a.source.node) &&
                 same_ref(node(a.body).source.node, a.source.node),
             "switch arm source belongs to another case");
        need(a.body == n.children[i + 1] &&
                 node(a.body).kind() == NodeKind::block &&
                 node(a.body).scope == a.scope &&
                 scope(a.scope).parent == n.scope,
             "switch arm body/scope");
        need(coverage.add(a.pattern), "unreachable switch arm");
      }
      const bool exhaustive = coverage.exhaustive();
      need(s->exhaustive && exhaustive, "nonexhaustive switch");
      return;
    }
    const auto &c = std::get<ConstraintCall>(n.data);
    auto owner = scope(n.scope).function;
    need(templates.count(owner.value),
         "constraint call survived into concrete execution");
    const TraitDecl *tr = trait_declaration(c.trait.declaration);
    need(tr, "missing constraint trait");
    auto owner_allowed_check = allowed(owner);
    check_trait_application(c.trait, owner_allowed_check);
    check_trait_parameter_bounds(c.trait, c.receiver);
    bool bounded = false;
    for (auto p : templates.at(owner.value)->params)
      if (p.trait && *p.trait == c.trait &&
          type(c.receiver).kind == TypeKind::TypeParameter &&
          type(c.receiver).declaration == p.symbol)
        bounded = true;
    need(bounded, "method is not authorized by template constraints");
    const MethodPrototype *method = nullptr;
    for (const auto &p : tr->methods)
      if (p.symbol == c.method)
        method = &p;
    need(method, "missing constraint method");
    auto written = source_method_call(n);
    need(written.has_value(), "constraint call lost its source method");
    need(written->name == symbol(c.method).name,
         "constraint call source method differs from target");
    const bool template_qualifier =
        template_trait_parameter(*written, owner);
    const auto owner_allowed = allowed(owner);
    valid_type(c.signature, owner_allowed);
    (void)constraint_method_arguments(n, c, *tr, *method);
    const auto &sig = type(c.signature);
    if (template_qualifier) {
      // T.method(...) has no synthetic receiver. Every declared parameter is
      // written explicitly from index 0, including a first ordinary Self.
      need(type(c.receiver).kind == TypeKind::TypeParameter,
           "T-qualified constraint receiver is not a type parameter");
      need(source_type_in_function(written->base, owner) == c.receiver,
           "T-qualified constraint source type differs from receiver");
      // A type-qualified call supplies every declared parameter explicitly.
      // It may use a static method with no Self, an actual first Self at
      // index zero (ordinary/move/inout), top-level move/inout, or a
      // method-own first parameter even when its concrete type equals Self.
      // Callback data is checked by the complete application substitution.
      const auto &raw = type(method->signature);
      for (const auto &p : raw.parameters)
        need(p.mode == ParamMode::ordinary || p.mode == ParamMode::move ||
                 p.mode == ParamMode::inout,
             "trait-static parameter mode is unsupported");
      need(n.type == sig.result && c.args.size() == sig.parameters.size() &&
               n.children.size() == c.args.size(),
           "trait-static call arity/children");
      std::set<uint32_t> seen;
      bool named = false;
      for (size_t i = 0; i < c.args.size(); ++i) {
        const auto &a = c.args[i];
        need(a.source_index == i &&
                 a.parameter_index < sig.parameters.size() &&
                 seen.insert(a.parameter_index).second,
             "trait-static argument mapping");
        need(n.children[i] == a.expression,
             "trait-static children must preserve source order");
        const auto p = sig.parameters[a.parameter_index];
        const auto &argument = node(a.expression);
        need(a.mode == p.mode && argument.type == p.type,
             "trait-static argument type/mode");
        if (p.mode == ParamMode::inout) {
          need(inout_place(argument) &&
                   inout_slot_type(m, p.type, owner_allowed),
               "trait-static inout requires a whole Woozy value place");
        } else {
          need(p.mode == ParamMode::ordinary || p.mode == ParamMode::move,
               "unsupported trait-static argument mode");
          expr(a.expression);
        }
        if (a.label) {
          named = true;
          need(*a.label == method->parameters[a.parameter_index].label,
               "trait-static label differs from trait prototype");
        } else {
          need(!named && a.parameter_index == i,
               "trait-static positional argument index/order");
        }
      }
      check_call_aliases(c.args, false);
      return;
    }
    if (written->base_symbol) {
      const auto kind = symbol(*written->base_symbol).kind;
      need(kind != modules::SymbolKind::type &&
               kind != modules::SymbolKind::builtin_type &&
               kind != modules::SymbolKind::type_parameter,
           "value-form constraint call has a type qualifier");
    }
    const auto &prototype = type(method->signature);
    need(!method->parameters.empty() && !prototype.parameters.empty(),
         "constraint method must declare Self");
    // Only the raw prototype Self creates a receiver. The written marker
    // below must match its declared mode before any concrete substitution.
    for (const auto &p : prototype.parameters)
      need(p.mode == ParamMode::ordinary || p.mode == ParamMode::move ||
               p.mode == ParamMode::inout,
           "unsupported constraint method parameter mode");
    const auto &raw_self = type(prototype.parameters.front().type);
    need(raw_self.kind == TypeKind::TypeParameter &&
             raw_self.declaration == tr->self,
         "constraint method self type must be the trait Self");
    need(n.type == sig.result && c.args.size() == sig.parameters.size() &&
             n.children.size() == c.args.size() && !c.args.empty(),
         "constraint call shape");
    // The receiver is a synthetic first argument. It must be evaluated first,
    // once, with its declared mode; explicit arguments follow in Source
    // order. Labels come from the real trait prototype, not a name match.
    const auto &receiver = c.args[0];
    need(receiver.source_index == 0 && receiver.parameter_index == 0 &&
             !receiver.label &&
             receiver.mode == prototype.parameters.front().mode &&
             receiver.expression == n.children[0] &&
             node(receiver.expression).type == c.receiver,
         "constraint receiver order/mode/type");
    check_receiver_source(n, c.args, written->base,
                          prototype.parameters.front().mode);
    std::set<uint32_t> seen{0};
    if (receiver.mode == ParamMode::inout) {
      const auto &place = node(receiver.expression);
      need(inout_place(place) &&
               inout_slot_type(m, place.type, owner_allowed),
           "constraint receiver inout requires a whole Woozy value place");
    } else {
      expr(receiver.expression);
    }
    bool named = false;
    for (size_t i = 1; i < c.args.size(); ++i) {
      const auto &a = c.args[i];
      need(a.source_index == i && a.parameter_index < sig.parameters.size() &&
               seen.insert(a.parameter_index).second,
           "constraint call argument mapping");
      need(n.children[i] == a.expression,
           "constraint call children must preserve source order");
      const auto p = sig.parameters[a.parameter_index];
      const auto &argument = node(a.expression);
      need(a.mode == p.mode && argument.type == p.type,
           "constraint argument type/mode");
      if (p.mode == ParamMode::inout) {
        // A concrete numeric/bool place is accepted. In a template the
        // substituted data type may remain the current constrained receiver
        // T or another enclosing template parameter; concrete instances and
        // calls still reject non-scalar data. Neither method-own nor Self
        // inout is replaced with Any or reduced to a name-only check.
        const auto &raw =
            type(method->signature).parameters[a.parameter_index];
        const bool trait_self_slot =
            i > 0 && raw.mode == ParamMode::inout &&
            type(raw.type).kind == TypeKind::TypeParameter &&
            type(raw.type).declaration == tr->self &&
            p.type == c.receiver;
        need(inout_place(argument) &&
                 (inout_slot_type(m, p.type, owner_allowed) ||
                  trait_self_slot),
             "constraint inout requires a whole Woozy value place");
      } else {
        need(p.mode == ParamMode::ordinary || p.mode == ParamMode::move,
             "unsupported constraint argument mode");
        expr(a.expression);
      }
      if (a.label) {
        named = true;
        need(*a.label == method->parameters[a.parameter_index].label,
             "constraint call label differs from trait prototype");
      } else {
        need(!named && a.parameter_index == i,
             "constraint positional argument index/order");
      }
    }
    check_call_aliases(c.args, false);
  }
  void instance_body(const HirFunction &origin, const HirFunction &f,
                     const Substitutions &sub,
                     const std::map<uint64_t, SymbolId> &renamed) const {
    std::map<uint64_t, HirNodeId> nodes;
    std::map<uint64_t, ScopeId> scopes;
    std::set<uint64_t> destinations;
    struct Work {
      HirNodeId origin, instance;
      bool capture_child = false;
    };
    std::vector<Work> work{{origin.body, f.body, false}};
    while (!work.empty()) {
      check_cancelled(stop);
      auto [a, b, capture_child] = work.back();
      work.pop_back();
      const auto &x = node(a), &y = node(b);
      need(nodes.emplace(a.value, b).second &&
               destinations.insert(b.value).second,
           "instance body is not a unique clone");
      const auto [it, inserted] = scopes.emplace(x.scope.value, y.scope);
      need(inserted || it->second == y.scope,
           "instance scope substitution mismatch");
      need(same_ref(x.source.node, y.source.node) &&
               x.source.span.snapshot == y.source.span.snapshot &&
               x.source.span.begin_byte == y.source.span.begin_byte &&
               x.source.span.end_byte == y.source.span.end_byte &&
               substitute(x.type, sub, substituted) == y.type &&
               x.children.size() == y.children.size(),
           "instance source/type/children differ from template");
      bool kind_ok =
          x.kind() == y.kind() ||
          (x.kind() == NodeKind::constraint_call &&
           y.kind() == NodeKind::call);
      if (!kind_ok && capture_child) {
        const auto *xm = std::get_if<Move>(&x.data);
        const auto *yr = std::get_if<Read>(&y.data);
        const auto *xr = std::get_if<Read>(&x.data);
        const auto *ym = std::get_if<Move>(&y.data);
        kind_ok = (xm && xm->binding && yr) || (xr && ym && ym->binding);
      }
      need(kind_ok, "instance node kind differs from template");
      bool captures = false;
      if (const auto *vx = std::get_if<FunctionValue>(&x.data)) {
        const auto *vy = std::get_if<FunctionValue>(&y.data);
        need(vy, "instance function value lost its payload");
        need(vx->move_capture == vy->move_capture &&
                 vx->bound.has_value() == vy->bound.has_value() &&
                 vx->captures.size() == vy->captures.size(),
             "instance function value capture metadata differs from template");
        bool symbol_ok = vx->symbol == vy->symbol;
        if (!symbol_ok) {
          for (const auto &candidate : m.instances)
            if (candidate.symbol == vy->symbol &&
                candidate.origin == vx->symbol) {
              symbol_ok = true;
              break;
            }
        }
        if (!symbol_ok && trait_prototype(vx->symbol) &&
            impl_method_exists(vy->symbol) &&
            symbol(vx->symbol).name == symbol(vy->symbol).name)
          symbol_ok = true;
        SymbolId vy_origin = vy->symbol;
        for (const auto &candidate : m.instances)
          if (candidate.symbol == vy->symbol) {
            vy_origin = candidate.origin;
            break;
          }
        if (!symbol_ok && trait_prototype(vx->symbol) &&
            impl_method_exists(vy_origin) &&
            symbol(vx->symbol).name == symbol(vy_origin).name)
          symbol_ok = true;
        need(symbol_ok, "instance function value target differs from template");
        for (size_t c = 0; c < vx->captures.size(); ++c) {
          const auto &before = vx->captures[c];
          const auto &after = vy->captures[c];
          auto mapped = renamed.find(before.source.value);
          need(before.origin == after.origin && mapped != renamed.end() &&
                   mapped->second == after.source &&
                   after.type == substitute(before.type, sub, substituted),
               "instance capture source/type differs from template");
          const bool dependent = type_contains_parameter(m, after.type);
          const bool owning =
              dependent || classify_value(m, after.type).needs_drop;
          need(after.move == (vy->move_capture && owning),
               "instance capture move intent differs from concrete type");
        }
        if (vx->bound)
          need(vy->bound->type ==
                       substitute(vx->bound->type, sub, substituted),
               "instance bound receiver type differs from template");
        captures = !vx->bound;
      }
      for (size_t i = 0; i < x.children.size(); ++i)
        work.push_back({x.children[i], y.children[i], captures});
    }
    auto local = [&](SymbolId id) {
      const auto it = renamed.find(id.value);
      need(it != renamed.end(),
           "instance metadata references a nonlocal template symbol");
      return it->second;
    };
    for (const auto &[a, b] : nodes) {
      check_cancelled(stop);
      const auto &x = node(HirNodeId{a}), &y = node(b);
      auto parent = scope(x.scope).parent;
      const auto &original_source = scope(x.scope).source;
      const auto &instance_source = scope(y.scope).source;
      need(same_ref(original_source.node, instance_source.node) &&
               original_source.span.snapshot == instance_source.span.snapshot &&
               original_source.span.begin_byte ==
                   instance_source.span.begin_byte &&
               original_source.span.end_byte == instance_source.span.end_byte,
           "instance scope source differs from template");
      need(
          scope(y.scope).parent ==
              (parent ? std::optional(scopes.at(parent->value)) : std::nullopt),
          "instance parent scope differs from template");
      // Errors and exits are derived records. Concrete catch matching can
      // differ from the template. effects_and_exits() independently validates
      // them against this instance's checked source structure and types.
      if (auto d = std::get_if<Defer>(&x.data)) {
        const auto &actual = std::get<Defer>(y.data);
        std::vector<SymbolId> refs;
        for (auto s : d->references)
          refs.push_back(local(s));
        std::sort(refs.begin(), refs.end(),
                  [](auto a, auto b) { return a.value < b.value; });
        need(actual.owner_scope == scopes.at(d->owner_scope.value) &&
                 actual.references == refs,
             "instance defer references were not renamed");
      }
      if (auto t = std::get_if<Try>(&x.data)) {
        const auto &actual = std::get<Try>(y.data);
        need(t->catches.size() == actual.catches.size(),
             "instance handler count");
        for (size_t i = 0; i < t->catches.size(); ++i) {
          const auto &c = t->catches[i], &v = actual.catches[i];
          need(v.error_type == substitute(c.error_type, sub, substituted) &&
                   v.binding == local(c.binding) &&
                   v.scope == scopes.at(c.scope.value) &&
                   v.body == nodes.at(c.body.value) &&
                   same_ref(v.source.node, c.source.node),
               "instance handler metadata was not substituted/renamed");
        }
      }
      // A constraint call may only become a direct call to the unique method
      // of the impl selected by its required trait and concrete receiver. A
      // same-signature method from another trait/impl is not interchangeable.
      if (auto cc = std::get_if<ConstraintCall>(&x.data)) {
        const auto *actual = std::get_if<Call>(&y.data);
        need(actual && actual->kind == CalleeKind::direct &&
                 actual->direct_symbol && actual->type_args.empty() &&
                 actual->signature ==
                     substitute(cc->signature, sub, substituted) &&
                 actual->args.size() == cc->args.size() &&
                 y.children.size() == cc->args.size(),
             "instance constraint call metadata");
        const auto receiver = substitute(cc->receiver, sub, substituted);
        std::vector<TypeId> required_args;
        for (auto a : cc->trait.args)
          required_args.push_back(substitute(a, sub, substituted));
        const ImplDecl *impl = nullptr;
        Substitutions impl_sub;
        for (const auto &im : m.impls) {
          if (!im.trait ||
              im.trait->declaration != cc->trait.declaration ||
              im.trait->args.size() != required_args.size())
            continue;
          // Select by Self and every ordered trait argument jointly, sharing
          // one substitution table so phantom-only impl parameters are bound.
          Substitutions match;
          if (!matches(im.self_type, receiver, match))
            continue;
          bool matched = true;
          for (size_t a = 0; a < required_args.size(); ++a)
            if (!matches(im.trait->args[a], required_args[a], match)) {
              matched = false;
              break;
            }
          if (!matched)
            continue;
          need(!impl, "ambiguous instance constraint impl");
          impl = &im;
          impl_sub = std::move(match);
        }
        need(impl, "instance constraint call has no impl");
        const TraitDecl *trait = trait_declaration(cc->trait.declaration);
        const MethodPrototype *prototype = nullptr;
        if (trait)
          for (const auto &p : trait->methods)
            if (p.symbol == cc->method) {
              need(!prototype, "duplicate instance constraint method");
              prototype = &p;
            }
        need(trait && prototype, "instance constraint prototype missing");
        std::optional<SymbolId> expected;
        for (auto method : impl->methods)
          if (symbol(method).name == symbol(prototype->symbol).name) {
            need(!expected, "duplicate instance impl method");
            expected = method;
          }
        need(expected.has_value(), "instance constraint impl method missing");
        auto method_args =
            constraint_method_arguments(x, *cc, *trait, *prototype);
        std::vector<TypeId> complete;
        for (const auto &p : impl->params) {
          auto it = std::find_if(impl_sub.begin(), impl_sub.end(),
                                 [&](auto entry) {
                                   return entry.first == p.symbol;
                                 });
          need(it != impl_sub.end(),
               "instance constraint impl argument is not determined");
          complete.push_back(it->second);
        }
        for (auto t : method_args.values)
          complete.push_back(substitute(t, sub, substituted));
        const FunctionInstance *target_instance = nullptr;
        for (const auto &i : m.instances)
          if (i.symbol == *actual->direct_symbol)
            target_instance = &i;
        if (target_instance) {
          need(target_instance->origin == *expected,
               "instance constraint target differs from selected trait impl");
          need(target_instance->type_args == complete,
               "instance constraint complete type arguments mismatch");
        } else {
          need(!templates.count(actual->direct_symbol->value),
               "instance constraint target retained a template");
          need(complete.empty(),
               "instance constraint generic method has no instance key");
          need(*actual->direct_symbol == *expected,
               "instance constraint target differs from selected trait impl");
        }
        for (size_t a = 0; a < cc->args.size(); ++a) {
          const auto &before = cc->args[a], &after = actual->args[a];
          need(before.source_index == after.source_index &&
                   before.parameter_index == after.parameter_index &&
                   before.label == after.label && before.mode == after.mode &&
                   after.expression == y.children[a],
               "instance constraint argument mapping/mode/order");
        }
      }
      if (auto r = std::get_if<Read>(&x.data)) {
        if (auto *read = std::get_if<Read>(&y.data))
          need(read->symbol == local(r->symbol),
               "instance read binding mismatch");
        else {
          const auto *move = std::get_if<Move>(&y.data);
          need(move && move->binding &&
                   *move->binding == local(r->symbol),
               "instance capture read/move conversion mismatch");
        }
      }
      if (auto c = std::get_if<ConstantLiteral>(&x.data)) {
        auto expected = c->value;
        auto actual = std::get<ConstantLiteral>(y.data).value;
        auto origin = [&](FunctionValue &v) {
          for (const auto &i : m.instances)
            if (i.symbol == v.symbol) {
              v.symbol = i.origin;
              v.type_args = i.type_args;
              break;
            }
        };
        transform_constant(expected, sub, substituted, origin);
        transform_constant(actual, {}, substituted, origin);
        need(same_constant_value(expected, actual),
             "instance constant differs from template");
      }
      if (auto r = std::get_if<Place>(&x.data))
        need(std::get<Place>(y.data).symbol == local(r->symbol),
             "instance place binding mismatch");
      if (auto r = std::get_if<Binding>(&x.data))
        need(std::get<Binding>(y.data).symbol == local(r->symbol),
             "instance declaration binding mismatch");
      if (auto r = std::get_if<Move>(&x.data)) {
        if (auto *move = std::get_if<Move>(&y.data))
          need(move->binding ==
                   (r->binding ? std::optional(local(*r->binding))
                               : std::nullopt),
               "instance move source mismatch");
        else {
          const auto *read = std::get_if<Read>(&y.data);
          need(r->binding && read &&
                   read->symbol == local(*r->binding),
               "instance capture move/read conversion mismatch");
        }
      }
      if (auto r = std::get_if<ForIn>(&x.data)) {
        const auto &v = std::get<ForIn>(y.data);
        need(v.element == local(r->element) &&
                 v.body == nodes.at(r->body.value) &&
                 v.body_scope == scopes.at(r->body_scope.value),
             "instance for-in identity mismatch");
      }
      if (auto r = std::get_if<Return>(&x.data))
        need(std::get<Return>(y.data).function == local(r->function),
             "instance return target mismatch");
      if (auto r = std::get_if<Break>(&x.data))
        need(std::get<Break>(y.data).loop == nodes.at(r->loop.value),
             "instance break target mismatch");
      if (auto r = std::get_if<Continue>(&x.data))
        need(std::get<Continue>(y.data).loop == nodes.at(r->loop.value),
             "instance continue target mismatch");
    }
  }

  void instance_records() {
    std::set<uint64_t> generated;
    // Resolver declarations precede generated copies. Retain their actual
    // SymbolIds so an instance copy with the same AST/name is not an origin.
    std::map<std::tuple<uint64_t, size_t, uint64_t>, SymbolId>
        original_declarations;
    for (const auto &s : m.resolved->symbols)
      if (s.definition) {
        const auto &r = *s.definition;
        original_declarations.emplace(
            std::make_tuple(r.module.value, r.file, r.node.value), s.id);
      }
    std::map<uint64_t, std::set<uint64_t>> template_symbols;
    std::set<uint64_t> capture_local_symbols;
    for (const auto &t : m.templates) {
      auto ps = parameter_set(t.params);
      need(!ps.empty(), "template without parameters");
      auto declared = source_generics(t.function.source);
      need(declared.size() == t.params.size(),
           "template source parameter count");
      for (size_t i = 0; i < declared.size(); ++i)
        need(declared[i] == t.params[i].symbol,
             "template parameter has wrong declaration owner");
      for (const auto &p : t.params) {
        need(p.source.node.module == t.function.source.node.module &&
                 p.source.node.file == t.function.source.node.file,
             "template parameter belongs to another source file");
      }
      const auto &ref = t.function.source.node;
      const auto &file =
          *m.resolved->project->module(ref.module).files[ref.file];
      auto &originals = template_symbols[t.function.symbol.value];
      std::vector<AstNodeId> pending{ref.node};
      while (!pending.empty()) {
        check_cancelled(stop);
        auto id = pending.back();
        pending.pop_back();
        const auto &n = file.node(id);
        // Function-type parameters have no variable declaration symbols.
        if (n.kind == AstKind::FunctionType)
          continue;
        if (id != ref.node &&
            (n.kind == AstKind::Function || n.kind == AstKind::FunctionLiteral))
          continue;
        if (id == ref.node || n.kind == AstKind::Parameter ||
            n.kind == AstKind::Binding || n.kind == AstKind::Catch ||
            n.kind == AstKind::ForIn ||
            (n.kind == AstKind::BindPattern &&
             file.tokens[n.token_begin].spelling != "_"))
          originals.insert(
              original_declarations
                  .at(std::make_tuple(ref.module.value, ref.file, id.value))
                  .value);
        pending.insert(pending.end(), n.children.begin(), n.children.end());
      }
      for (const auto &capture : t.function.captures) {
        originals.insert(capture.local.value);
        capture_local_symbols.insert(capture.local.value);
      }
      need(originals.count(t.function.symbol.value),
           "template function is not its original declaration");
      for (auto sid : originals)
        if (sid != t.function.symbol.value)
          need(locals.count(sid) &&
                   locals.at(sid).function == t.function.symbol,
               "template HIR omits or replaces a real source binding");
    }
    std::set<std::pair<uint64_t, std::vector<uint64_t>>> keys;
    for (const auto &i : m.instances) {
      need(instances.emplace(i.symbol.value, &i).second,
           "duplicate instance identity");
      auto it = templates.find(i.origin.value);
      need(it != templates.end(), "instance origin is not a template");
      const auto &t = *it->second;
      need(i.target == m.config && i.type_args.size() == t.params.size(),
           "instance target/arity");
      Substitutions sub;
      std::vector<uint64_t> args;
      for (size_t a = 0; a < i.type_args.size(); ++a) {
        valid_type(i.type_args[a], {});
        sub.push_back({t.params[a].symbol, i.type_args[a]});
        args.push_back(i.type_args[a].value);
      }
      need(keys.insert({i.origin.value, args}).second,
           "duplicate logical instance");
      constraints(t.params, i.type_args);
      const auto &f = function(i.symbol);
      need(!templates.count(i.symbol.value) &&
               f.signature ==
                   substitute(t.function.signature, sub, substituted),
           "instance signature substitution mismatch");
      need(same_ref(f.source.node, t.function.source.node),
           "instance function has forged AST source");
      const auto &originals = template_symbols.at(i.origin.value);
      for (const auto &[sid, l] : locals)
        if (l.function == i.origin)
          need(originals.count(sid),
               "template local is not an original symbol");
      need(i.renamed.size() == originals.size(),
           "instance map omits original template symbols");
      std::map<uint64_t, SymbolId> renamed;
      std::set<uint64_t> dest;
      for (auto [from, to] : i.renamed) {
        need(from != to && renamed.emplace(from.value, to).second &&
                 dest.insert(to.value).second &&
                 generated.insert(to.value).second,
             "instance renamed map is not one-to-one");
        const auto &a = symbol(from), &b = symbol(to);
        need(originals.count(from.value),
             "renamed source is not an original template-owned symbol");
        if (capture_local_symbols.count(from.value)) {
          need(a.kind == modules::SymbolKind::value &&
                   b.kind == modules::SymbolKind::value &&
                   a.name == b.name && a.module == b.module &&
                   !a.definition && !b.definition && !a.declaration &&
                   !b.declaration,
               "capture local rename is not a definitionless value copy");
          continue;
        }
        need(a.definition &&
                 original_declarations.at(std::make_tuple(
                     a.definition->module.value, a.definition->file,
                     a.definition->node.value)) == from,
             "renamed source is not an original template-owned symbol");
        need(a.kind == b.kind && a.name == b.name && a.module == b.module &&
                 a.definition && b.definition &&
                 same_ref(*a.definition, *b.definition) && a.declaration &&
                 b.declaration &&
                 a.declaration->snapshot == b.declaration->snapshot &&
                 a.declaration->begin_byte == b.declaration->begin_byte &&
                 a.declaration->end_byte == b.declaration->end_byte &&
                 a.exported == b.exported && a.target == b.target,
             "generated symbol has forged origin/source");
        need(a.definition->module == t.function.source.node.module &&
                 a.definition->file == t.function.source.node.file &&
                 a.declaration &&
                 a.declaration->begin_byte >=
                     t.function.source.span.begin_byte &&
                 a.declaration->end_byte <= t.function.source.span.end_byte,
             "renamed symbol belongs to another template");
      }
      need(renamed.count(i.origin.value) &&
               renamed.at(i.origin.value) == i.symbol,
           "instance function missing from renamed map");
      need(f.parameters.size() == t.function.parameters.size(),
           "instance parameter arity");
      for (size_t p = 0; p < f.parameters.size(); ++p)
        need(renamed.count(t.function.parameters[p].symbol.value) &&
                 renamed.at(t.function.parameters[p].symbol.value) ==
                     f.parameters[p].symbol,
             "instance parameter mapping mismatch");
      need(f.captures.size() == t.function.captures.size(),
           "instance capture count differs from template");
      for (size_t c = 0; c < f.captures.size(); ++c) {
        const auto &before = t.function.captures[c];
        const auto &after = f.captures[c];
        need(before.origin == after.origin &&
                 renamed.count(before.local.value) &&
                 renamed.at(before.local.value) == after.local &&
                 substitute(before.type, sub, substituted) == after.type,
             "instance capture binding mapping mismatch");
      }
      // Every local and pattern symbol belongs to this instance's map.
      for (const auto &[sid, l] : locals)
        if (l.function == i.symbol)
          need(dest.count(sid), "instance local absent from renamed map");
      for (auto sid : dest) {
        if (sid == i.symbol.value)
          continue;
        need(locals.count(sid) && locals.at(sid).function == i.symbol,
             "renamed symbol is not owned by its instance");
      }
      instance_body(t.function, f, sub, renamed);
    }
    for (const auto &f : m.functions)
      if (!source_generics(f.source).empty())
        need(instances.count(f.symbol.value),
             "generic-source concrete function has no instance record");
    for (const auto &f : m.functions)
      for (const auto &t : m.templates)
        if (same_ref(f.source.node, t.function.source.node))
          need(instances.count(f.symbol.value) &&
                   instances.at(f.symbol.value)->origin == t.function.symbol,
               "concrete template body has no instance origin record");
    for (const auto &im : m.impls)
      if (im.trait) {
        const TraitDecl *tr = trait_declaration(im.trait->declaration);
        need(tr && im.methods.size() == tr->methods.size(),
             "impl method count");
        need(im.trait->args.size() == tr->params.size(),
             "impl trait application arity");
        for (const auto &p : tr->methods) {
          const HirFunction *f = nullptr;
          for (auto id : im.methods)
            if (symbol(id).name == symbol(p.symbol).name) {
              need(!f, "duplicate impl method");
              f = &function(id);
            }
          need(f, "impl method missing");
          auto all = source_parameters(f->source);
          need(all.size() >= im.params.size(),
               "impl method source lost its impl generic prefix");
          for (size_t i = 0; i < im.params.size(); ++i)
            need(all[i].symbol == im.params[i].symbol,
                 "impl method prefix differs from impl parameters");
          std::vector<GenericParam> own(all.begin() + im.params.size(),
                                        all.end());
          auto prototype_own = source_parameters(p.source);
          need(own.size() == prototype_own.size(),
               "impl method own generic count mismatch");
          Substitutions alpha{{tr->self, im.self_type}};
          for (size_t i = 0; i < tr->params.size(); ++i)
            alpha.push_back({tr->params[i].symbol, im.trait->args[i]});
          // Complete the positional own-parameter map first, then compare
          // each bound after substituting its ordered trait arguments.
          for (size_t i = 0; i < own.size(); ++i) {
            Type parameter;
            parameter.kind = TypeKind::TypeParameter;
            parameter.declaration = own[i].symbol;
            alpha.push_back({prototype_own[i].symbol,
                             substituted.intern(parameter)});
          }
          for (size_t i = 0; i < own.size(); ++i) {
            need(static_cast<bool>(own[i].trait) ==
                     static_cast<bool>(prototype_own[i].trait),
                 "impl method generic bound presence mismatch");
            if (prototype_own[i].trait) {
              TraitRef expected = *prototype_own[i].trait;
              for (auto &arg : expected.args)
                arg = substitute(arg, alpha, substituted);
              need(expected == *own[i].trait,
                   "impl method generic bound mismatch");
            }
          }
          need(f->signature == substitute(p.signature, alpha, substituted),
               "impl method signature mismatch");
          need(f->parameters.size() == p.parameters.size(),
               "impl method parameter count mismatch");
          for (size_t i = 0; i < f->parameters.size(); ++i)
            need(f->parameters[i].label == p.parameters[i].label,
                 "impl method parameter labels mismatch");
        }
      }
  }

  void tree(const HirFunction &f) {
    struct Frame {
      HirNodeId id;
      size_t next;
    };
    std::vector<Frame> stack;
    std::vector<HirNodeId> loops, break_targets;
    auto push = [&](HirNodeId id, std::optional<HirNodeId> parent) {
      check_cancelled(stop);
      const auto &n = node(id);
      need(!owners[id.value], "HIR node cycle or shared executable child");
      owners[id.value] = f.symbol;
      parents[id.value] = parent;
      enter[id.value] = ++clock;
      need(n.source.node.module == f.source.node.module &&
               n.source.node.file == f.source.node.file &&
               n.source.span.begin_byte >= f.source.span.begin_byte &&
               n.source.span.end_byte <= f.source.span.end_byte,
           "node source belongs to another function");
      const auto &s = scope(n.scope);
      need(s.function == f.symbol, "node scope belongs to another function");
      if (!parent) {
        need(n.kind() == NodeKind::block && n.scope == f.scope && !s.parent,
             "function root block/scope");
      } else {
        const auto &p = node(*parent);
        if (n.kind() == NodeKind::block || n.kind() == NodeKind::for_ ||
            n.kind() == NodeKind::for_in)
          need(s.parent && *s.parent == p.scope,
               "nested scope must have structural parent");
        else
          need(n.scope == p.scope,
               "node scope differs from enclosing construct");
        if (n.kind() == NodeKind::place || n.kind() == NodeKind::field_place ||
            n.kind() == NodeKind::index_place || foreign_access_place(n)) {
          bool allowed = p.kind() == NodeKind::assign && p.children[0] == id;
          if (p.kind() == NodeKind::call ||
              p.kind() == NodeKind::constraint_call) {
            const auto &args = p.kind() == NodeKind::call
                                   ? std::get<Call>(p.data).args
                                   : std::get<ConstraintCall>(p.data).args;
            for (const auto &a : args)
              allowed =
                  allowed || (a.mode == ParamMode::inout && a.expression == id);
          }
          allowed = allowed ||
                    (p.kind() == NodeKind::field_place && p.children[0] == id);
          need(allowed, "place used outside assignment or inout argument");
        }
      }
      if (n.kind() == NodeKind::block || n.kind() == NodeKind::for_ ||
          n.kind() == NodeKind::for_in) {
        need(!scope_used[n.scope.value],
             "scope has multiple owning constructs");
        scope_used[n.scope.value] = true;
        scope_nodes[n.scope.value] = id;
      }
      if (n.kind() == NodeKind::for_ || n.kind() == NodeKind::for_in)
        loops.push_back(id);
      if (n.kind() == NodeKind::for_ || n.kind() == NodeKind::for_in ||
          n.kind() == NodeKind::switch_)
        break_targets.push_back(id);
      if (n.kind() == NodeKind::switch_) {
        for (const auto &a : std::get<Switch>(n.data).arms)
          register_pattern(a.pattern, a.scope, f.symbol, enter[id.value]);
      }
      if (auto t = std::get_if<Try>(&n.data))
        for (const auto &c : t->catches)
          need(locals
                   .emplace(c.binding.value,
                            Local{c.error_type, c.scope, f.symbol, false,
                                  enter[id.value]})
                   .second,
               "duplicate catch binding identity");
      if (auto fi = std::get_if<ForIn>(&n.data))
        need(locals
                 .emplace(fi->element.value,
                          Local{iteration_element(node(n.children[0]).type),
                                fi->body_scope, f.symbol, false,
                                enter[id.value]})
                 .second,
             "duplicate for-in element");
      if (n.kind() == NodeKind::return_) {
        need(std::get<Return>(n.data).function == f.symbol,
             "return target function mismatch");
        const auto &sig = type(f.signature);
        // An async function's source `return` produces the Task payload; the
        // published signature result is the Task handle itself.
        TypeId result_type = sig.result;
        if (f.async) {
          const auto &task = type(result_type);
          need(task.kind == TypeKind::Task && task.type_args.size() == 2,
               "async function result is not a Task");
          result_type = task.type_args.at(0);
        }
        need(n.children.empty()
                     ? type(result_type).kind == TypeKind::Unit
                     : expr(n.children[0]).type == result_type,
             "function return type mismatch");
      }
      if (n.kind() == NodeKind::break_ || n.kind() == NodeKind::continue_) {
        auto target = n.kind() == NodeKind::break_
                          ? std::get<Break>(n.data).loop
                          : std::get<Continue>(n.data).loop;
        const auto &targets =
            n.kind() == NodeKind::break_ ? break_targets : loops;
        need((node(target).kind() == NodeKind::for_ ||
              node(target).kind() == NodeKind::for_in ||
              (n.kind() == NodeKind::break_ &&
               node(target).kind() == NodeKind::switch_)) &&
                 !targets.empty() && targets.back() == target,
             "jump target must be nearest enclosing loop in same function");
        need(scope(node(target).scope).function == f.symbol &&
                 ancestor(node(target).scope, n.scope),
             "jump function/scope mismatch");
      }
      stack.push_back({id, 0});
    };
    push(f.body, {});
    while (!stack.empty()) {
      check_cancelled(stop);
      auto &frame = stack.back();
      const auto &n = node(frame.id);
      if (frame.next < n.children.size()) {
        auto child = n.children[frame.next++];
        auto parent = frame.id;
        push(child, parent);
        continue;
      }
      leave[frame.id.value] = ++clock;
      if (n.kind() == NodeKind::binding) {
        auto b = std::get<Binding>(n.data);
        const auto &s = symbol(b.symbol);
        declaration(b.symbol, n.source, s.kind, AstKind::Binding);
        const auto &file =
            *m.resolved->project->modules[n.source.node.module.value]
                 .files[n.source.node.file];
        const auto &ast = file.node(n.source.node.node);
        need(ast.token_begin < file.tokens.size(), "binding token range");
        auto keyword = file.tokens[ast.token_begin].spelling;
        need(keyword == "let" || keyword == "var" || keyword == "const",
             "local binding keyword");
        need(!b.writable || keyword == "var",
             "non-var binding marked writable");
        need(s.kind != modules::SymbolKind::constant || !b.writable,
             "constant marked writable");
        need(locals
                 .emplace(b.symbol.value,
                          Local{node(n.children[0]).type, n.scope, f.symbol,
                                b.writable, leave[frame.id.value]})
                 .second,
             "duplicate local/parameter identity");
      }
      if (n.kind() == NodeKind::for_ || n.kind() == NodeKind::for_in)
        loops.pop_back();
      if (n.kind() == NodeKind::for_ || n.kind() == NodeKind::for_in ||
          n.kind() == NodeKind::switch_)
        break_targets.pop_back();
      stack.pop_back();
    }
  }
  // New source error/cleanup constructs cannot silently disappear from HIR.
  void source_effect_nodes(const HirFunction &f) const {
    const auto &ref = f.source.node;
    const auto &file =
        *m.resolved->project->module(ref.module).files.at(ref.file);
    std::function<bool(AstNodeId)> type_syntax = [&](AstNodeId id) {
      const auto &a = file.node(id);
      if (a.kind == AstKind::FunctionType || a.kind == AstKind::NamedType)
        return true;
      if (a.kind == AstKind::BracketItem || a.kind == AstKind::Parenthesized ||
          a.kind == AstKind::ParenthesizedType)
        return type_syntax(a.children.at(0));
      auto ref = f.source.node;
      ref.node = id;
      if (a.kind == AstKind::NeutralBracket)
        return type_syntax(a.children.at(0));
      for (const auto &o : m.resolved->occurrences)
        if (same_ref(o.source, ref)) {
          auto k = symbol(o.symbol).kind;
          return k == modules::SymbolKind::type ||
                 k == modules::SymbolKind::builtin_type ||
                 k == modules::SymbolKind::type_parameter;
        }
      return false;
    };
    auto index_syntax = [&](const AstNode &a) {
      if (a.kind != AstKind::NeutralBracket)
        return false;
      bool types = a.children.size() > 1;
      for (size_t i = 1; i < a.children.size(); ++i)
        types &= type_syntax(a.children[i]);
      if (types)
        return false;
      auto ref = f.source.node;
      ref.node = a.children.at(0);
      for (const auto &o : m.resolved->occurrences)
        if (same_ref(o.source, ref)) {
          auto k = symbol(o.symbol).kind;
          return k != modules::SymbolKind::function &&
                 k != modules::SymbolKind::type &&
                 k != modules::SymbolKind::builtin_type &&
                 k != modules::SymbolKind::type_parameter;
        }
      return true;
    };
    std::map<uint64_t, size_t> expected, actual;
    std::set<uint64_t> expected_foreign_reads, expected_foreign_calls;
    std::set<uint64_t> actual_foreign_reads, actual_foreign_calls;
    std::map<uint64_t, std::set<uint64_t>> node_types;
    for (const auto &n : m.nodes)
      if (scope(n.scope).function == f.symbol)
        node_types[n.source.node.node.value].insert(n.type.value);
    auto unwrap_source = [&](AstNodeId id) {
      for (;;) {
        const auto &a = file.node(id);
        if (a.kind != AstKind::Parenthesized &&
            a.kind != AstKind::BracketItem &&
            !(a.kind == AstKind::Unary &&
              file.tokens.at(a.token_begin).spelling == "+"))
          return id;
        need(!a.children.empty(), "transparent source wrapper has no operand");
        id = a.children[0];
      }
    };
    auto source_symbol = [&](AstNodeId id) -> std::optional<SymbolId> {
      auto r = ref;
      r.node = unwrap_source(id);
      for (const auto &o : m.resolved->occurrences)
        if (same_ref(o.source, r))
          return o.symbol;
      return {};
    };
    auto local_type = [&](AstNodeId id) -> std::optional<TypeId> {
      auto sid = source_symbol(id);
      if (!sid)
        return {};
      for (const auto &instance : m.instances)
        if (instance.symbol == f.symbol)
          for (auto [from, to] : instance.renamed)
            if (from == *sid) {
              sid = to;
              break;
            }
      auto local = locals.find(sid->value);
      if (local == locals.end() || local->second.function != f.symbol)
        return {};
      return local->second.type;
    };
    auto member_name = [&](AstNodeId id) {
      const auto &a = file.node(id);
      need(a.token_end > a.token_begin, "member source token range");
      return file.tokens[a.token_end - 1].spelling;
    };
    auto common_result = [&](const std::vector<ForeignBindingId> &candidates)
        -> std::optional<TypeId> {
      std::optional<TypeId> result;
      for (auto candidate : candidates) {
        need(candidate.value < m.foreign_bindings.size(),
             "foreign candidate index");
        const auto &binding = m.foreign_bindings[candidate.value];
        need(binding.signature.has_value(), "foreign candidate signature");
        auto type = this->type(*binding.signature).result;
        if (result && *result != type)
          return {};
        result = type;
      }
      return result;
    };
    auto method_candidates = [&](TypeId receiver, std::string_view name) {
      std::vector<ForeignBindingId> result;
      const auto &t = type(receiver);
      if (t.kind != TypeKind::ForeignOpaque)
        return result;
      const auto &owner = m.resolved->project->foreign_binding(t.binding);
      const auto *decl = std::get_if<modules::BindingType>(&owner.detail);
      if (!decl)
        return result;
      for (auto id : decl->members) {
        const auto &member = m.resolved->project->foreign_binding(id);
        const auto *call = std::get_if<modules::BindingCallable>(&member.detail);
        if (call && call->call_kind == "method" && member.name == name)
          result.push_back(id);
      }
      return result;
    };
    auto callable_candidates = [&](TypeId receiver) {
      std::vector<ForeignBindingId> result;
      const auto &t = type(receiver);
      if (t.kind != TypeKind::ForeignOpaque)
        return result;
      const auto &owner = m.resolved->project->foreign_binding(t.binding);
      const auto *decl = std::get_if<modules::BindingType>(&owner.detail);
      if (!decl)
        return result;
      for (auto id : decl->members) {
        const auto &member = m.resolved->project->foreign_binding(id);
        const auto *call = std::get_if<modules::BindingCallable>(&member.detail);
        if (call && call->call_kind == "callable")
          result.push_back(id);
      }
      return result;
    };
    std::function<std::optional<TypeId>(AstNodeId, unsigned)> ast_type;
    ast_type = [&](AstNodeId id, unsigned depth) -> std::optional<TypeId> {
      if (depth > 64)
        return {};
      id = unwrap_source(id);
      const auto &a = file.node(id);
      if (a.kind == AstKind::Name || a.kind == AstKind::Member) {
        if (auto local = local_type(id))
          return local;
        if (auto sid = source_symbol(id))
          if (auto ex = m.resolved->foreign_export(*sid);
              ex && ex->declaration &&
              ex->declaration->value < m.foreign_bindings.size())
            return m.foreign_bindings[ex->declaration->value].type;
        if (a.kind == AstKind::Member) {
          auto base = ast_type(a.children.at(0), depth + 1);
          if (base && type(*base).kind == TypeKind::ForeignOpaque) {
            auto candidates = method_candidates(*base, member_name(id));
            if (!candidates.empty())
              return m.foreign_bindings[candidates.front().value].type;
            const auto &owner = m.resolved->project->foreign_binding(
                type(*base).binding);
            for (auto member : std::get<modules::BindingType>(owner.detail).properties)
              if (m.resolved->project->foreign_binding(member).name ==
                  member_name(id))
                return m.foreign_bindings[member.value].type;
          }
          if (base && type(*base).kind == TypeKind::NominalStruct) {
            const auto &d = nominal_type(*base);
            for (const auto &field : d.fields)
              if (symbol(field.symbol).name == member_name(id))
                return field_type(*base, field);
          }
        }
        auto found = node_types.find(id.value);
        if (found != node_types.end() && found->second.size() == 1)
          return TypeId{*found->second.begin()};
        return {};
      }
      if (a.kind == AstKind::Call && !a.children.empty()) {
        auto callee = a.children[0];
        auto target = unwrap_source(callee);
        const auto &t = file.node(target);
        if (auto sid = source_symbol(target)) {
          if (auto ex = m.resolved->foreign_export(*sid);
              ex && !ex->candidates.empty())
            return common_result(ex->candidates);
          if (symbol(*sid).kind == modules::SymbolKind::function) {
            auto function = functions.find(sid->value);
            if (function != functions.end())
              return type(function->second->signature).result;
          }
        }
        auto method = target;
        if (t.kind == AstKind::NeutralBracket && !t.children.empty())
          method = unwrap_source(t.children[0]);
        const auto &mnode = file.node(method);
        if (mnode.kind == AstKind::Member && !source_symbol(method)) {
          auto base = ast_type(mnode.children.at(0), depth + 1);
          if (base && type(*base).kind == TypeKind::ForeignOpaque) {
            auto candidates = method_candidates(*base, member_name(method));
            if (!candidates.empty())
              return common_result(candidates);
          }
        }
        if (auto callee_type = ast_type(callee, depth + 1)) {
          if (type(*callee_type).kind == TypeKind::Function)
            return type(*callee_type).result;
          if (type(*callee_type).kind == TypeKind::ForeignOpaque) {
            auto candidates = callable_candidates(*callee_type);
            if (!candidates.empty())
              return common_result(candidates);
          }
        }
        auto found = node_types.find(id.value);
        if (found != node_types.end() && found->second.size() == 1)
          return TypeId{*found->second.begin()};
        return {};
      }
      if (a.kind == AstKind::NeutralBracket && a.children.size() == 2) {
        auto base = ast_type(a.children.at(0), depth + 1);
        if (base && type(*base).kind == TypeKind::ForeignOpaque) {
          const auto &owner = m.resolved->project->foreign_binding(
              type(*base).binding);
          const auto &indices = std::get<modules::BindingType>(owner.detail).indexers;
          if (!indices.empty())
            return m.foreign_bindings[indices.front().value].type;
        }
        if (base && (type(*base).kind == TypeKind::Vec ||
                     type(*base).kind == TypeKind::Slice))
          return type(*base).type_args.at(0);
      }
      if (a.kind == AstKind::Slice) {
        auto base = ast_type(a.children.at(0), depth + 1);
        if (base && type(*base).kind == TypeKind::ForeignOpaque) {
          const auto &owner = m.resolved->project->foreign_binding(
              type(*base).binding);
          const auto &slices = std::get<modules::BindingType>(owner.detail).slices;
          if (!slices.empty())
            return m.foreign_bindings[slices.front().value].type;
        }
        if (base && (type(*base).kind == TypeKind::Vec ||
                     type(*base).kind == TypeKind::Slice))
          return *base;
      }
      return {};
    };
    std::vector<AstNodeId> todo{ref.node};
    while (!todo.empty()) {
      check_cancelled(stop);
      auto id = todo.back();
      todo.pop_back();
      const auto &n = file.node(id);
      if (id != ref.node &&
          (n.kind == AstKind::Function || n.kind == AstKind::FunctionLiteral ||
           n.kind == AstKind::FunctionType))
        continue;
      if (n.kind == AstKind::Call && !n.children.empty()) {
        auto callee = n.children[0];
        auto target = unwrap_source(callee);
        const auto &t = file.node(target);
        bool foreign = false;
        if (auto sid = source_symbol(target))
          if (auto ex = m.resolved->foreign_export(*sid);
              ex && !ex->candidates.empty()) {
            expected_foreign_reads.insert(target.value);
            foreign = true;
          }
        auto method = target;
        if (t.kind == AstKind::NeutralBracket && !t.children.empty())
          method = unwrap_source(t.children[0]);
        const auto &mnode = file.node(method);
        if (!foreign && mnode.kind == AstKind::Member && !source_symbol(method)) {
          auto base = ast_type(mnode.children.at(0), 0);
          if (base && type(*base).kind == TypeKind::ForeignOpaque) {
            expected_foreign_reads.insert(unwrap_source(method).value);
            foreign = true;
          }
        }
        if (!foreign) {
          auto callee_type = ast_type(callee, 0);
          if (callee_type && type(*callee_type).kind == TypeKind::ForeignOpaque) {
            expected_foreign_reads.insert(target.value);
            foreign = true;
          }
        }
        if (foreign)
          expected_foreign_calls.insert(id.value);
      }
      if (n.kind == AstKind::Name || n.kind == AstKind::Member) {
        auto key = unwrap_source(id);
        if (auto sid = source_symbol(id))
          if (auto ex = m.resolved->foreign_export(*sid);
              ex && !ex->c_type && !ex->c_callable &&
              (ex->kind == modules::ForeignExportKind::value ||
               ex->kind == modules::ForeignExportKind::function))
            expected_foreign_reads.insert(key.value);
        if (n.kind == AstKind::Member && !source_symbol(id)) {
          auto base = ast_type(n.children.at(0), 0);
          if (base && type(*base).kind == TypeKind::ForeignOpaque)
            expected_foreign_reads.insert(key.value);
        }
      }
      if (n.kind == AstKind::Try || n.kind == AstKind::Defer ||
          n.kind == AstKind::Throw || n.kind == AstKind::Catch ||
          n.kind == AstKind::List || n.kind == AstKind::Slice ||
          n.kind == AstKind::ForIn || index_syntax(n) ||
          (n.kind == AstKind::Unary &&
           file.tokens[n.token_begin].spelling == "move"))
        ++expected[id.value];
      todo.insert(todo.end(), n.children.begin(), n.children.end());
    }
    std::vector<HirNodeId> pending{f.body};
    while (!pending.empty()) {
      check_cancelled(stop);
      const auto &n = node(pending.back());
      pending.pop_back();
      const auto kind = file.node(n.source.node.node).kind;
      if (n.kind() == NodeKind::foreign_read) {
        const auto &read = std::get<ForeignRead>(n.data);
        if (!read.access || *read.access == ForeignAccessKind::property_read ||
            *read.access == ForeignAccessKind::property_place)
          actual_foreign_reads.insert(unwrapped(n.source.node).node.value);
      }
      if (auto call = std::get_if<Call>(&n.data);
          call && call->kind == CalleeKind::foreign)
        actual_foreign_calls.insert(n.source.node.node.value);
      if (kind == AstKind::Try || kind == AstKind::Defer ||
          kind == AstKind::Throw || kind == AstKind::List ||
          kind == AstKind::Slice || kind == AstKind::ForIn ||
          index_syntax(file.node(n.source.node.node)) ||
          (kind == AstKind::Unary &&
           file.tokens[file.node(n.source.node.node).token_begin].spelling ==
               "move")) {
        need((kind == AstKind::Try && n.kind() == NodeKind::try_) ||
                 (kind == AstKind::Defer && n.kind() == NodeKind::defer_) ||
                 (kind == AstKind::Throw && n.kind() == NodeKind::throw_) ||
                 (kind == AstKind::List && n.kind() == NodeKind::vec_init) ||
                 (kind == AstKind::Slice &&
                  (n.kind() == NodeKind::vec_slice ||
                   (n.kind() == NodeKind::foreign_read &&
                    std::get<ForeignRead>(n.data).access &&
                    (std::get<ForeignRead>(n.data).access ==
                         ForeignAccessKind::slice_read ||
                     std::get<ForeignRead>(n.data).access ==
                         ForeignAccessKind::slice_place)))) ||
                 (kind == AstKind::ForIn && n.kind() == NodeKind::for_in) ||
                 (kind == AstKind::Unary &&
                  (n.kind() == NodeKind::move ||
                   (n.kind() == NodeKind::function_value &&
                    std::get<FunctionValue>(n.data).move_capture))) ||
                 (index_syntax(file.node(n.source.node.node)) &&
                  (n.kind() == NodeKind::index_read ||
                   n.kind() == NodeKind::index_place ||
                   (n.kind() == NodeKind::foreign_read &&
                    std::get<ForeignRead>(n.data).access &&
                    (std::get<ForeignRead>(n.data).access ==
                         ForeignAccessKind::index_read ||
                     std::get<ForeignRead>(n.data).access ==
                         ForeignAccessKind::index_place)))),
             "source effect construct was erased or replaced");
        ++actual[n.source.node.node.value];
      }
      if (auto t = std::get_if<Try>(&n.data))
        for (const auto &c : t->catches)
          ++actual[c.source.node.node.value];
      pending.insert(pending.end(), n.children.begin(), n.children.end());
    }
    need(actual_foreign_reads == expected_foreign_reads,
         "HIR foreign reads differ from source operations");
    need(actual_foreign_calls == expected_foreign_calls,
         "HIR foreign calls differ from source operations");
    need(actual == expected,
         "HIR omits or duplicates source error/cleanup constructs");
    bool owning = false;
    for (const auto &n : m.nodes)
      if (scope(n.scope).function == f.symbol)
        owning |= n.kind() == NodeKind::move || n.kind() == NodeKind::for_in ||
                  // A function value is owning for classification/IR, but it
                  // adds no source owning-expression/cleanup statement. Keep
                  // the existing scalar-builder source gate unchanged.
                  (n.kind() != NodeKind::function_value &&
                   classify_value(m, n.type).needs_drop);
    if (expected.empty() && !owning)
      return; // preserve existing scalar builder lowering
    const auto &syntax_function = file.node(ref.node);
    need(!syntax_function.children.empty(), "function has no source body");
    auto exact_child = [&](HirNodeId child, AstNodeId syntax) {
      auto expected_ref = ref;
      const auto &expected = file.node(syntax);
      if (expected.kind == AstKind::Unsafe) {
        need(expected.children.size() == 1 &&
                 file.node(expected.children[0]).kind == AstKind::Block,
             "unsafe source has no single body block");
        expected_ref.node = expected.children[0];
      } else {
        expected_ref.node = syntax;
      }
      need(same_ref(node(child).source.node, expected_ref),
           "HIR statement source order/control owner differs from AST");
    };
    exact_child(f.body, syntax_function.children.back());
    // Check statements, not expression equivalence: wrappers/constant folding
    // inside expressions remain legal. Synthetic else-if and Case Blocks use
    // the same source forms as the producer's established lowering.
    std::vector<HirNodeId> statements{f.body};
    while (!statements.empty()) {
      check_cancelled(stop);
      const auto id = statements.back();
      statements.pop_back();
      const auto &n = node(id);
      const auto &syntax = file.node(n.source.node.node);
      auto kind = [&](AstKind expected_kind) {
        need(syntax.kind == expected_kind,
             "statement kind differs from its AST source");
      };
      switch (n.kind()) {
      case NodeKind::block: {
        std::vector<AstNodeId> sequence;
        if (syntax.kind == AstKind::Block)
          sequence = syntax.children;
        else if (syntax.kind == AstKind::Case) {
          const bool def =
              file.tokens.at(syntax.token_begin).spelling == "default";
          sequence.assign(syntax.children.begin() + (def ? 0 : 1),
                          syntax.children.end());
        } else if (syntax.kind == AstKind::If)
          sequence.push_back(n.source.node.node);
        else
          fail("lexical block has no supported AST statement sequence");
        need(n.children.size() == sequence.size(),
             "HIR omits or adds a source statement");
        for (size_t c = 0; c < sequence.size(); ++c)
          exact_child(n.children[c], sequence[c]);
        break;
      }
      case NodeKind::if_:
        kind(AstKind::If);
        need(n.children.size() == syntax.children.size(),
             "if source branch count");
        for (size_t branch = 1; branch < n.children.size(); ++branch)
          exact_child(n.children[branch], syntax.children[branch]);
        break;
      case NodeKind::for_in:
        kind(AstKind::ForIn);
        exact_child(n.children[1], syntax.children.back());
        break;
      case NodeKind::for_:
        kind(AstKind::For);
        exact_child(n.children[3], syntax.children.back());
        break;
      case NodeKind::switch_: {
        kind(AstKind::Switch);
        const auto &arms = std::get<Switch>(n.data).arms;
        need(syntax.children.size() == arms.size() + 1,
             "switch source arm count");
        for (size_t a = 0; a < arms.size(); ++a)
          exact_child(arms[a].body, syntax.children[a + 1]);
        break;
      }
      case NodeKind::binding:
        kind(AstKind::Binding);
        break;
      case NodeKind::assign:
        kind(AstKind::Assign);
        break;
      case NodeKind::expression_statement:
        kind(AstKind::ExpressionStatement);
        break;
      case NodeKind::return_:
        kind(AstKind::Return);
        break;
      case NodeKind::break_:
        kind(AstKind::Break);
        break;
      case NodeKind::continue_:
        kind(AstKind::Continue);
        break;
      case NodeKind::noop:
        kind(AstKind::For);
        break; // synthetic init/step only
      // effect_shape already checks these bodies and handler source ownership.
      case NodeKind::try_:
        kind(AstKind::Try);
        break;
      case NodeKind::defer_:
        kind(AstKind::Defer);
        break;
      case NodeKind::throw_:
        kind(AstKind::Throw);
        break;
      default:
        break;
      }
      statements.insert(statements.end(), n.children.begin(), n.children.end());
    }
  }

  struct FunctionSource {
    SymbolId function;
    std::vector<TypeId> type_args;
    bool explicit_type_args = false;
  };

  // Rebuild the function selected by a const initializer from its real Source.
  // The public type does not include a callee's internal specialization, but
  // the selected origin and ordered explicit type arguments still identify
  // the represented FunctionValue. Only existing const routes are followed.
  FunctionSource constant_function_source(SymbolId id) const {
    std::set<uint64_t> visited;
    // Alias chains have no syntax nesting limit; only repeated symbols cycle.
    for (;;) {
      check_cancelled(stop);
      const auto &s = symbol(id);
      need(s.kind == modules::SymbolKind::constant && s.definition,
           "constant function source is not a real constant");
      need(visited.insert(id.value).second, "constant function source cycle");
      auto declaration = *s.definition;
      const auto &file = *m.resolved->project->module(declaration.module)
                              .files.at(declaration.file);
      const auto &binding = file.node(declaration.node);
      need(binding.kind == AstKind::Binding && !binding.children.empty(),
           "constant function source is not a binding");
      auto initializer = declaration;
      initializer.node = binding.children.back();
      initializer = unwrapped(initializer);
      const auto &ast = file.node(initializer.node);
      if (ast.kind == AstKind::FunctionLiteral) {
        std::optional<SymbolId> function;
        for (const auto &candidate : m.resolved->symbols)
          if (candidate.kind == modules::SymbolKind::function &&
              candidate.definition &&
              same_ref(*candidate.definition, initializer))
            function = candidate.id;
        need(function.has_value(),
             "constant function literal has no resolved symbol");
        return {*function, {}, false};
      }
      if (ast.kind == AstKind::NeutralBracket) {
        need(!ast.children.empty(),
             "constant generic function value has no target");
        auto base = initializer;
        base.node = ast.children.front();
        auto function = resolved_source(base);
        need(function && symbol(*function).kind == modules::SymbolKind::function,
             "constant generic initializer is not a function");
        std::vector<TypeId> type_args;
        for (size_t i = 1; i < ast.children.size(); ++i) {
          auto argument = initializer;
          argument.node = ast.children[i];
          type_args.push_back(source_type(argument));
        }
        return {*function, std::move(type_args), true};
      }
      need(ast.kind == AstKind::Name || ast.kind == AstKind::Member,
           "constant function initializer has an unsupported source kind");
      auto target = resolved_source(initializer);
      need(target.has_value(),
           "constant function initializer has no resolved symbol");
      const auto &selected = symbol(*target);
      if (selected.kind == modules::SymbolKind::function)
        return {*target, {}, false};
      need(selected.kind == modules::SymbolKind::constant,
           "constant initializer is neither a function nor a constant");
      id = *target;
    }
  }

  // A function value is a source-level reference to an executable function,
  // not an arbitrary value of a matching function type. Keep the existing
  // constant routes separate, but require a Name/Member source that really
  // resolves to a function to keep its original symbol (or that symbol's
  // instance origin). Const sources instead compare the selected function
  // origin and ordered explicit type arguments with their real initializer.
  // This rejects a corrupted FunctionValue whose type and signature are
  // unchanged but whose callback implementation or specialization was replaced.
  FunctionValue source_lambda_value(modules::NodeRef r, SymbolId owner) const {
    std::optional<SymbolId> original;
    // As in instance_records(), resolver declarations precede generated copies.
    for (const auto &s : m.resolved->symbols)
      if (s.kind == modules::SymbolKind::function && s.definition &&
          same_ref(*s.definition, r)) {
        original = s.id;
        break;
      }
    need(original.has_value(), "function literal has no original declaration");
    const auto target = templates.find(original->value);
    if (target == templates.end())
      return {*original, {}};
    std::vector<TypeId> arguments;
    for (const auto &p : target->second->params)
      arguments.push_back(parameter_type(p.symbol));
    if (templates.count(owner.value))
      return {*original, std::move(arguments)};
    const auto environment = instances.find(owner.value);
    need(environment != instances.end(),
         "generic function literal has no enclosing instance");
    const auto &instance = *environment->second;
    const auto &parameters = templates.at(instance.origin.value)->params;
    Substitutions sub;
    for (size_t i = 0; i < parameters.size(); ++i)
      sub.emplace_back(parameters[i].symbol, instance.type_args.at(i));
    for (auto &argument : arguments)
      argument = substitute(argument, sub, substituted);
    for (const auto &candidate : m.instances)
      if (candidate.origin == *original && candidate.type_args == arguments)
        return {candidate.symbol, {}};
    fail("function literal has no matching enclosing specialization");
  }

  // Independent free-variable recomputation: collect value occurrences in
  // source order whose declaration lies outside the literal's AST subtree.
  std::vector<SymbolId> capture_origins(modules::NodeRef literal) const {
    const auto &file =
        *m.resolved->project->module(literal.module).files.at(literal.file);
    std::set<uint64_t> subtree;
    std::vector<AstNodeId> pending{literal.node};
    while (!pending.empty()) {
      check_cancelled(stop);
      auto id = pending.back();
      pending.pop_back();
      if (!subtree.insert(id.value).second)
        continue;
      const auto &node = file.node(id);
      pending.insert(pending.end(), node.children.begin(), node.children.end());
    }
    std::vector<const modules::Occurrence *> found;
    for (const auto &occurrence : m.resolved->occurrences) {
      const auto &ref = occurrence.source;
      if (ref.module != literal.module || ref.file != literal.file ||
          !subtree.count(ref.node.value))
        continue;
      if (symbol(occurrence.symbol).kind != modules::SymbolKind::value)
        continue;
      found.push_back(&occurrence);
    }
    std::sort(found.begin(), found.end(), [](const auto *a, const auto *b) {
      if (a->span.begin_byte != b->span.begin_byte)
        return a->span.begin_byte < b->span.begin_byte;
      return a->source.node.value < b->source.node.value;
    });
    std::vector<SymbolId> result;
    std::set<uint64_t> seen;
    for (const auto *occurrence : found) {
      const auto &declaration = symbol(occurrence->symbol);
      bool inside = false;
      if (declaration.definition &&
          declaration.definition->module == literal.module &&
          declaration.definition->file == literal.file)
        inside = subtree.count(declaration.definition->node.value) != 0;
      if (inside || !seen.insert(occurrence->symbol.value).second)
        continue;
      result.push_back(occurrence->symbol);
    }
    return result;
  }
  SymbolId capture_source_origin(SymbolId id) const {
    for (const auto &instance : m.instances)
      for (auto [from, to] : instance.renamed)
        if (to == id)
          return from;
    return id;
  }
  const CaptureBinding *function_capture(const HirFunction &f,
                                         SymbolId origin) const {
    for (const auto &capture : f.captures)
      if (capture.origin == origin)
        return &capture;
    return nullptr;
  }
  // Recompute one literal function value's capture schema from the real
  // source tree and the independently verified lambda entry. Submitted
  // captures/move_capture are compared, never trusted.
  void check_literal_function_value(const HirNode &n, const FunctionValue &v,
                                    modules::NodeRef literal_ref) const {
    const auto owner = scope(n.scope).function;
    const auto expected = source_lambda_value(literal_ref, owner);
    need(v.symbol == expected.symbol && v.type_args == expected.type_args,
         "function literal identity differs from its enclosing "
         "specialization");
    auto target = functions.find(v.symbol.value);
    need(target != functions.end() && target->second,
         "capturing function value has no HIR lambda entry");
    const auto &lambda = *target->second;
    need(lambda.captures.size() == v.captures.size(),
         "function value capture count differs from the lambda entry");
    need(n.children.size() == v.captures.size(),
         "function value capture child count");
    const auto creator = functions.find(owner.value);
    need(creator != functions.end() && creator->second,
         "function value creator has no HIR entry");
    const auto &creator_function = *creator->second;
    for (size_t i = 0; i < v.captures.size(); ++i) {
      const auto &source = v.captures[i];
      const auto &binding = lambda.captures[i];
      need(source.origin == binding.origin && source.type == binding.type,
           "function value capture order/origin/type differs from the lambda "
           "entry");
      const bool dependent = type_contains_parameter(m, source.type);
      const bool owning =
          dependent || classify_value(m, source.type).needs_drop;
      const bool expected_move = v.move_capture && owning;
      need(source.move == expected_move,
           "capture move intent differs from the real function value mode");
      if (!expected_move && !dependent &&
          !ordinary_copyable(m, source.type))
        need(false, "ordinary function capture type is not copyable");
      if (const auto *outer = function_capture(creator_function,
                                               source.origin)) {
        need(source.source == outer->local,
             "capture source is not the creator's matching capture local");
      } else {
        need(capture_source_origin(source.source) ==
                 capture_source_origin(source.origin),
             "capture source is not the creator binding for its origin");
        auto found = locals.find(source.source.value);
        need(found != locals.end() && found->second.function == owner,
             "capture source is not a creator-local binding");
      }
      const auto &child = node(n.children[i]);
      need(child.type == source.type, "capture child type");
      if (expected_move) {
        const auto *move = std::get_if<Move>(&child.data);
        need(move && move->binding &&
                 *move->binding == source.source &&
                 child.children.empty(),
             "owning capture child is not a synthetic source move");
      } else {
        const auto *read = std::get_if<Read>(&child.data);
        need(read && read->symbol == source.source &&
                 child.children.empty(),
             "copied capture child is not a creator source read");
      }
    }
  }
  // A bound value method stores one receiver child. Its callable signature is
  // the real method signature without the first Self parameter.
  void check_bound_method_value(const HirNode &n,
                                const FunctionValue &v) const {
    need(v.captures.empty() && !v.move_capture && v.bound.has_value(),
         "bound function value has inconsistent capture metadata");
    need(n.children.size() == 1, "bound function value receiver count");
    const auto &receiver = node(n.children[0]);
    need(receiver.type == v.bound->type,
         "bound receiver type differs from its schema");
    const auto full_id = instantiated_signature(v.symbol, v.type_args);
    const auto &full = type(full_id);
    need(!full.parameters.empty(), "bound method has no Self parameter");
    const auto self = full.parameters.front();
    need(self.type == v.bound->type && self.mode == v.bound->mode &&
             (self.mode == ParamMode::ordinary ||
              self.mode == ParamMode::move),
         "bound method Self type/mode differs from its declaration");
    need(ordinary_copyable(m, v.bound->type),
         "bound receiver type is not ordinary-copyable");
    Type trimmed = full;
    trimmed.parameters.erase(trimmed.parameters.begin());
    need(type(n.type) == trimmed, "bound method value type mismatch");
    auto origin = v.symbol;
    if (auto instance = instances.find(v.symbol.value);
        instance != instances.end())
      origin = instance->second->origin;
    const auto *impl = method_impl(origin);
    Substitutions match;
    need(std::find(impl->methods.begin(), impl->methods.end(), origin) !=
             impl->methods.end() &&
             matches(impl->self_type, v.bound->type, match),
         "bound method receiver does not select its impl");
    const auto &file = *m.resolved->project->module(n.source.node.module)
                            .files.at(n.source.node.file);
    auto source = n.source.node;
    auto ast = file.node(source.node);
    while (ast.kind == AstKind::Parenthesized) {
      need(!ast.children.empty(), "bound method source wrapper");
      source.node = ast.children.at(0);
      ast = file.node(source.node);
    }
    need(ast.kind == AstKind::Member && ast.children.size() == 1,
         "bound method source is not a value Member");
    const auto name = std::string(file.tokens[ast.token_end - 1].spelling);
    need(symbol(origin).name == name,
         "bound method source name differs from its target");
    auto base = source;
    base.node = ast.children.front();
    auto base_ast = file.node(base.node);
    while (base_ast.kind == AstKind::Parenthesized) {
      need(!base_ast.children.empty(), "bound receiver wrapper");
      base.node = base_ast.children.at(0);
      base_ast = file.node(base.node);
    }
    need(same_ref(receiver.source.node, base),
         "bound receiver child differs from the Member base");
    if (base_ast.kind == AstKind::Unary &&
        file.tokens.at(base_ast.token_begin).spelling == "move") {
      const auto *move = std::get_if<Move>(&receiver.data);
      need(move && move->binding,
           "move-bound receiver is not a source binding move");
    } else {
      need(!std::get_if<Move>(&receiver.data),
           "ordinary bound receiver was replaced by a move");
    }
  }
  void function_value_source(const HirNode &n) const {
    auto origin = [&](SymbolId id) {
      for (const auto &i : m.instances)
        if (i.symbol == id)
          return i.origin;
      return id;
    };
    auto value = std::get_if<FunctionValue>(&n.data);
    if (!value)
      return;
    const auto &file = *m.resolved->project->module(n.source.node.module)
                            .files.at(n.source.node.file);
    auto r = n.source.node;
    auto ast = file.node(r.node);
    while (ast.kind == AstKind::Parenthesized ||
           ast.kind == AstKind::BracketItem ||
           ast.kind == AstKind::NeutralBracket) {
      need(!ast.children.empty(), "function value source wrapper");
      r.node = ast.children.at(0);
      ast = file.node(r.node);
    }
    bool move_literal = false;
    if (ast.kind == AstKind::Unary &&
        file.tokens.at(ast.token_begin).spelling == "move") {
      need(ast.children.size() == 1, "move func source arity");
      move_literal = true;
      r.node = ast.children.front();
      ast = file.node(r.node);
      while (ast.kind == AstKind::Parenthesized ||
             ast.kind == AstKind::BracketItem ||
             ast.kind == AstKind::NeutralBracket) {
        need(!ast.children.empty(), "move func literal wrapper");
        r.node = ast.children.front();
        ast = file.node(r.node);
      }
    }
    if (ast.kind == AstKind::FunctionLiteral) {
      need(value->move_capture == move_literal,
           "function value move_capture differs from its real source");
      need(!value->bound,
           "literal function value cannot also bind a receiver");
      check_literal_function_value(n, *value, r);
      return;
    }
    need(!move_literal, "move func source is not a function literal");
    need(!value->move_capture,
         "plain function value cannot claim move capture");
    if (value->bound) {
      check_bound_method_value(n, *value);
      return;
    }
    need(value->captures.empty(),
         "captured function value has no verified literal source");
    if (auto method = source_method_member(n.source.node)) {
      if (auto prototype = trait_prototype(value->symbol))
        check_template_method_value_source(n, *value, *prototype->first,
                                           *prototype->second, *method);
      else
        check_concrete_method_value_source(n, *value, *method);
      return;
    }
    // A FunctionValue whose target is an impl method or raw prototype can
    // never fall through the generic non-function-source skip. Its legal
    // non-Member Source is a constant alias; everything else is corrupt.
    const auto prototype = trait_prototype(value->symbol);
    if (prototype || method_value_is_impl(*value)) {
      auto source = constant_method_source(n);
      need(source.has_value(),
           "method value source has no method or constant declaration");
      need(!prototype,
           "trait prototype method value source is not a bounded T method");
      check_concrete_method_value_source(n, *value, *source,
                                         source->base.module, SymbolId{});
      return;
    }
    std::optional<SymbolId> source_symbol;
    for (const auto &o : m.resolved->occurrences)
      if (same_ref(o.source, r))
        source_symbol = o.symbol;
    if (source_symbol &&
        symbol(*source_symbol).kind == modules::SymbolKind::constant) {
      const auto expected = constant_function_source(*source_symbol);
      auto actual_function = origin(value->symbol);
      auto actual_args = value->type_args;
      auto instance = instances.find(value->symbol.value);
      if (instance != instances.end()) {
        actual_function = instance->second->origin;
        actual_args = instance->second->type_args;
      }
      need(actual_function == expected.function,
           "constant function value differs from its real initializer "
           "function or ordered type arguments");
      need(!expected.explicit_type_args || actual_args == expected.type_args,
           "constant function value differs from its real initializer "
           "function or ordered type arguments");
      return;
    }
    // Other supported value producers retain their existing source checks;
    // this rule only decides the function-name route.
    if (!source_symbol ||
        symbol(*source_symbol).kind != modules::SymbolKind::function)
      return;
    need(origin(value->symbol) == *source_symbol,
         "function value source identity differs from resolver");
  }
  void function_value_sources() const {
    for (const auto &n : m.nodes)
      function_value_source(n);
  }

  void check_constant(const HirNode &n, const ConstantValue &value) const {
    need(value.type == n.type, "constant root type differs from expression");
    size_t work = 0;
    std::function<void(const ConstantValue &, size_t)> shape =
        [&](const ConstantValue &v, size_t depth) {
          check_cancelled(stop);
          need(depth <= 128 && ++work <= 100000,
               "constant value exceeds budget");
          valid_type(v.type, allowed(scope(n.scope).function));
          const auto t = type(v.type);
          if (auto a = std::get_if<ConstantAggregate>(&v.data)) {
            need(t.kind == TypeKind::NominalStruct ||
                     t.kind == TypeKind::NominalEnum,
                 "constant aggregate type");
            const auto &d = nominal_type(v.type);
            const auto *fields = &d.fields;
            if (t.kind == TypeKind::NominalEnum) {
              fields = nullptr;
              for (const auto &variant : d.variants)
                if (a->variant == variant.symbol)
                  fields = &variant.payload;
            } else
              need(!a->variant, "variant on constant struct");
            need(fields && a->fields.size() == fields->size(),
                 "constant payload arity/variant");
            for (size_t i = 0; i < a->fields.size(); ++i) {
              need(a->fields[i].type == field_type(v.type, (*fields)[i]),
                   "constant field type/order");
              shape(a->fields[i], depth + 1);
            }
          } else if (auto i = std::get_if<IntegerLiteral>(&v.data)) {
            need(integer(t.kind), "constant integer type");
            mpz_class number(i->decimal);
            const auto width = bit_width(t, m.config);
            mpz_class mod = mpz_class(1) << width;
            mpz_class low =
                signed_integer(t.kind) ? mpz_class(-(mod >> 1)) : mpz_class(0);
            mpz_class high = signed_integer(t.kind) ? mpz_class(mod >> 1) : mod;
            need(number.get_str() == i->decimal && number >= low &&
                     number < high,
                 "constant integer range/canonical value");
            if (number < 0)
              number += mod;
            need(number.get_ui() == i->bits, "constant integer bits");
          } else if (auto s = std::get_if<StringLiteral>(&v.data)) {
            need(t.kind == TypeKind::String, "constant string type");
            validate_utf8(s->utf8);
          } else if (auto f = std::get_if<FloatLiteral>(&v.data))
            need(t.kind == TypeKind::Float &&
                     (t.bits == 64 || f->bits >> 32 == 0),
                 "constant float bits/type");
          else if (auto f = std::get_if<FunctionValue>(&v.data)) {
            need(t.kind == TypeKind::Function, "constant function type");
            // Constants carry no environment and no receiver adapter.
            need(f->captures.empty() && !f->move_capture && !f->bound,
                 "constant function value cannot capture or bind a receiver");
            if (trait_prototype(f->symbol))
              need(templates.count(scope(n.scope).function.value),
                   "constant trait prototype survived concrete execution");
            else
              need(instantiated_signature(f->symbol, f->type_args) == v.type,
                   "constant function signature");
          } else if (std::holds_alternative<BoolLiteral>(v.data))
            need(t.kind == TypeKind::Bool, "constant bool type");
          else
            need(t.kind == TypeKind::Unit, "constant Unit type");
        };
    shape(value, 0);
    auto expected = source_constant_value(
        m, substituted, n.source.node, n.type,
        [&](modules::NodeRef r) {
          return source_type_in_function(r, scope(n.scope).function);
        },
        [&](modules::NodeRef r, TypeId signature) {
          // Reuse the existing independent function/method identity checks.
          // Equal signatures alone never choose a different implementation.
          auto try_value = [&](FunctionValue v) -> bool {
            try {
              if (!trait_prototype(v.symbol) &&
                  instantiated_signature(v.symbol, v.type_args) != signature)
                return false;
              HirNode candidate{signature,
                                {r, m.resolved->project->module(r.module)
                                        .files.at(r.file)
                                        ->node(r.node)
                                        .span},
                                n.scope,
                                {},
                                v,
                                {},
                                {}};
              function_value_source(candidate);
              auto ref = r;
              const auto &file =
                  *m.resolved->project->module(r.module).files.at(r.file);
              while (file.node(ref.node).kind == AstKind::Parenthesized)
                ref.node = file.node(ref.node).children.at(0);
              if (file.node(ref.node).kind == AstKind::NeutralBracket &&
                  !source_method_member(r)) {
                std::vector<TypeId> written;
                for (size_t i = 1; i < file.node(ref.node).children.size();
                     ++i) {
                  auto a = ref;
                  a.node = file.node(ref.node).children[i];
                  written.push_back(
                      source_type_in_function(a, scope(n.scope).function));
                }
                auto actual = v.type_args;
                if (auto found = instances.find(v.symbol.value);
                    found != instances.end())
                  actual = found->second->type_args;
                need(actual == written,
                     "constant function explicit arguments differ from source");
              }
              return true;
            } catch (const Invalid &) {
              return false;
            }
          };
          auto literal = unwrapped(r);
          if (m.resolved->project->module(literal.module)
                  .files.at(literal.file)
                  ->node(literal.node)
                  .kind == AstKind::FunctionLiteral) {
            auto value = source_lambda_value(literal, scope(n.scope).function);
            need(try_value(value),
                 "constant function literal signature/source mismatch");
            return value;
          }
          for (const auto &f : m.functions)
            if (try_value({f.symbol, {}}))
              return FunctionValue{f.symbol, {}};
          for (const auto &t : m.templates) {
            auto ref = r;
            const auto &file =
                *m.resolved->project->module(r.module).files.at(r.file);
            while (file.node(ref.node).kind == AstKind::Parenthesized)
              ref.node = file.node(ref.node).children.at(0);
            std::vector<TypeId> arguments;
            if (file.node(ref.node).kind == AstKind::NeutralBracket)
              for (size_t i = 1; i < file.node(ref.node).children.size(); ++i) {
                auto a = ref;
                a.node = file.node(ref.node).children[i];
                arguments.push_back(
                    source_type_in_function(a, scope(n.scope).function));
              }
            FunctionValue v{t.function.symbol, arguments};
            if (arguments.size() == t.params.size() && try_value(v))
              return v;
          }
          for (const auto &trait : m.traits)
            for (const auto &method : trait.methods) {
              FunctionValue v{method.symbol, {}};
              if (try_value(v))
                return v;
            }
          fail("constant function has no matching real source identity");
        },
        stop);
    if (!same_constant_value(value, expected)) {
      std::function<std::string(const ConstantValue &, const ConstantValue &)>
          difference = [&](const auto &a, const auto &b) -> std::string {
        if (a.type != b.type)
          return " type " + std::to_string(a.type.value) + "/" +
                 std::to_string(b.type.value);
        if (auto f = std::get_if<FunctionValue>(&a.data)) {
          auto g = std::get_if<FunctionValue>(&b.data);
          return " function " + std::to_string(f->symbol.value) + "/" +
                 (g ? std::to_string(g->symbol.value) : "other");
        }
        if (auto fields = std::get_if<ConstantAggregate>(&a.data))
          if (auto other = std::get_if<ConstantAggregate>(&b.data))
            for (size_t i = 0;
                 i < std::min(fields->fields.size(), other->fields.size()); ++i)
              if (!same_constant_value(fields->fields[i], other->fields[i]))
                return " field " + std::to_string(i) +
                       difference(fields->fields[i], other->fields[i]);
        return {};
      };
      fail("constant value differs from real source" +
               difference(value, expected),
           n.source.span);
    }
  }

  // A FunctionValue whose real source names an exported constant publishes
  // that constant's final inferred function type. Check every represented use
  // against the same visibility rule as the source checker; internal callee
  // specialization metadata is not part of this public type. An unused
  // constant has no HIR node; only the checker can inspect its inferred type.
  void public_constant_values() const {
    for (const auto &n : m.nodes) {
      if (!std::holds_alternative<FunctionValue>(n.data) &&
          !std::holds_alternative<ConstantLiteral>(n.data))
        continue;
      auto source_symbol = resolved_source(n.source.node);
      if (!source_symbol)
        continue;
      const auto &s = symbol(*source_symbol);
      if (s.kind != modules::SymbolKind::constant || !s.exported)
        continue;
      auto reject = [&](SymbolId private_type) {
        const auto &p = symbol(private_type);
        fail("public constant value uses private type: " + p.name,
             p.declaration);
      };
      if (auto p = detail::private_interface_type(m.types, *m.resolved, n.type))
        reject(*p);
    }
  }

  void ownership_sources() const {
    std::set<uint64_t> owning_functions;
    for (const auto &n : m.nodes)
      if (n.kind() == NodeKind::move || n.kind() == NodeKind::for_in ||
          (n.kind() != NodeKind::function_value &&
           classify_value(m, n.type).needs_drop))
        owning_functions.insert(scope(n.scope).function.value);
    auto origin = [&](SymbolId id) {
      for (const auto &i : m.instances)
        for (auto [from, to] : i.renamed)
          if (to == id)
            return from;
      return id;
    };
    auto capture_local_for = [&](SymbolId owner,
                                 SymbolId occurrence)
        -> std::optional<SymbolId> {
      auto found = functions.find(owner.value);
      if (found == functions.end() || !found->second)
        return std::nullopt;
      if (const auto *capture =
              function_capture(*found->second, occurrence))
        return capture->local;
      return std::nullopt;
    };
    for (size_t i = 0; i < m.nodes.size(); ++i) {
      const auto &n = m.nodes[i];
      const auto &file = *m.resolved->project->module(n.source.node.module)
                              .files.at(n.source.node.file);
      auto r = n.source.node;
      auto ast = file.node(r.node);
      auto unwrap = [&] {
        while (ast.kind == AstKind::Parenthesized ||
               ast.kind == AstKind::BracketItem) {
          r.node = ast.children.at(0);
          ast = file.node(r.node);
        }
      };
      auto source_input = [&](HirNodeId input, AstNodeId syntax,
                              const char *message) {
        const auto &value = node(input);
        auto expected = n.source.node;
        expected.node = syntax;
        // Only transparent wrappers may disappear during type checking.
        // A nested subexpression of a call is not the endpoint itself.
        for (;;) {
          if (same_ref(value.source.node, expected))
            return;
          const auto &source = file.node(expected.node);
          const bool wrapper =
              source.kind == AstKind::Parenthesized ||
              source.kind == AstKind::BracketItem ||
              (source.kind == AstKind::Unary &&
               file.tokens.at(source.token_begin).spelling == "+");
          need(
              wrapper && source.children.size() == 1,
              message);
          expected.node = source.children[0];
        }
      };
      if (n.kind() == NodeKind::assign) {
        need(ast.kind == AstKind::Assign && ast.children.size() == 2,
             "assignment source is not a real Assign with two children");
        source_input(n.children[0], ast.children[0],
                     "assignment LHS differs from the real Assign left child");
        source_input(n.children[1], ast.children[1],
                     "assignment RHS differs from the real Assign right child");
      }
      std::optional<SymbolId> read;
      if (auto p = std::get_if<Read>(&n.data);
          p && classify_value(m, n.type).needs_drop)
        read = p->symbol;
      // A local function-constant alias stays a real Read, so its function
      // identity is not present in the node payload. Either side may reveal
      // that this position is a const read: the immutable Source decides the
      // real binding, while the submitted payload must be the same symbol
      // after an actual instance rename. A changed payload must not be able to
      // turn the check off by claiming to be an ordinary let/parameter.
      std::optional<SymbolId> function_constant_read;
      if (auto p = std::get_if<Read>(&n.data);
          p && type(n.type).kind == TypeKind::Function) {
        const auto payload = origin(p->symbol);
        const auto source = resolved_source(n.source.node);
        const bool payload_constant =
            symbol(payload).kind == modules::SymbolKind::constant;
        const bool source_constant =
            source && symbol(*source).kind == modules::SymbolKind::constant;
        if (payload_constant || source_constant) {
          need(source && *source == payload,
               "function constant read source identity differs from "
               "resolver");
          // A const chain may end in a Type.method source, which has no
          // ordinary occurrence symbol; else it ends in a free function,
          // literal or ordered generic suffix. Both routes are rebuilt from
          // the real Binding AST, not from a checker-maintained table.
          if (!constant_method_source(n))
            (void)constant_function_source(payload);
          function_constant_read = payload;
        }
      }
      if (auto mv = std::get_if<Move>(&n.data)) {
        bool synthetic_capture = false;
        if (mv->binding && parents[i]) {
          const auto &parent = node(*parents[i]);
          if (parent.kind() == NodeKind::function_value) {
            const auto &value = std::get<FunctionValue>(parent.data);
            if (value.move_capture)
              for (const auto &capture : value.captures)
                if (capture.move && capture.source == *mv->binding)
                  synthetic_capture = true;
          }
        }
        if (synthetic_capture) {
          const auto local = locals.find(mv->binding->value);
          need(local != locals.end() && !local->second.read_only,
               "synthetic capture move source is not a mutable creator "
               "binding");
          read = mv->binding;
          need(ast.kind == AstKind::Name,
               "synthetic capture move source is not a name");
        } else {
        need(ast.kind == AstKind::Unary &&
                 file.tokens.at(ast.token_begin).spelling == "move",
             "move lost source marker");
        r.node = ast.children.at(0);
        ast = file.node(r.node);
        unwrap();
        // The immutable source chooses the move category, not the submitted
        // payload. A Name operand always moves its actual local binding.
        if (ast.kind == AstKind::Name) {
          std::optional<SymbolId> expected;
          for (const auto &o : m.resolved->occurrences)
            if (same_ref(o.source, r))
              expected = o.symbol;
          need(expected.has_value(), "move binding has no resolved source");
          const auto kind = symbol(*expected).kind;
          need(kind == modules::SymbolKind::value ||
                   kind == modules::SymbolKind::constant,
               "move source is not a local binding");
          const auto owner = scope(n.scope).function;
          for (const auto &instance : m.instances)
            if (instance.symbol == owner)
              for (auto [from, to] : instance.renamed)
                if (from == *expected) {
                  expected = to;
                  break;
                }
          const auto local = locals.find(expected->value);
          need(local != locals.end() && local->second.function == owner &&
                   mv->binding == expected,
               "source binding move was replaced or has wrong instance "
               "identity");
          read = expected;
        } else {
          need(!mv->binding, "computed move source cannot name a binding");
          need(n.children.size() == 1, "move temporary source");
          source_input(n.children[0], r.node,
                       "computed move operand differs from source");
        }
        }
      }
      if (read) {
        unwrap();
        std::optional<SymbolId> actual;
        for (const auto &o : m.resolved->occurrences)
          if (same_ref(o.source, r))
            actual = o.symbol;
        const auto owner = scope(n.scope).function;
        if (actual) {
          if (auto mapped = capture_local_for(owner, *actual))
            need(*read == *mapped,
                 "owning read/move capture local identity differs from the "
                 "lambda mapping");
          else
            need(*actual == origin(*read),
                 "owning read/move source identity differs from resolver");
        } else
          need(false, "owning read/move source identity differs from resolver");
      }
      if (function_constant_read) {
        auto actual = resolved_source(n.source.node);
        need(actual && *actual == *function_constant_read,
             "function constant read source identity differs from resolver");
        need(symbol(*actual).kind == modules::SymbolKind::constant &&
                 symbol(*actual).definition,
             "function constant read source is not a real const declaration");
      }
      if (owning_functions.count(scope(n.scope).function.value)) {
        auto within = [&](HirNodeId value, AstNodeId input) {
          const auto &a = file.node(input);
          const auto &v = node(value);
          need(v.source.span.begin_byte >= a.span.begin_byte &&
                   v.source.span.end_byte <= a.span.end_byte,
               "owning expression source evaluation order");
        };
        if (n.kind() == NodeKind::binary ||
            n.kind() == NodeKind::short_circuit) {
          need(ast.kind == AstKind::Binary && ast.children.size() == 2,
               "owning expression binary source");
          within(n.children[0], ast.children[0]);
          within(n.children[1], ast.children[1]);
        }
        if (n.kind() == NodeKind::field_read ||
            n.kind() == NodeKind::field_place) {
          need(ast.kind == AstKind::Member, "owning field source");
          within(n.children[0], ast.children[0]);
        }
        if (auto b = std::get_if<BoolLiteral>(&n.data)) {
          if (ast.kind == AstKind::Literal)
            need(file.tokens.at(ast.token_begin).spelling ==
                     (b->value ? "true" : "false"),
                 "branch boolean differs from source literal");
          else if (ast.kind == AstKind::Name || ast.kind == AstKind::Member) {
            bool constant = false;
            for (const auto &o : m.resolved->occurrences)
              if (same_ref(o.source, n.source.node))
                constant =
                    symbol(o.symbol).kind == modules::SymbolKind::constant;
            need(constant, "branch condition replaced a live source variable "
                           "with a constant");
          } else
            need(ast.kind == AstKind::For && b->value,
                 "unexpected synthetic branch boolean");
        }
        if (auto c = std::get_if<Call>(&n.data)) {
          need(ast.kind == AstKind::Call && !ast.children.empty(),
               "owning call lost source call");
          size_t offset = c->args.size() + 1 == ast.children.size() ? 0 : 1;
          need(c->args.size() + 1 == ast.children.size() + offset,
               "owning call source arity");
          for (size_t j = offset; j < c->args.size(); ++j) {
            auto a = file.node(ast.children[j - offset + 1]);
            need(a.kind == AstKind::Argument && a.children.size() == 1,
                 "owning call source argument");
            within(c->args[j].expression, a.children[0]);
          }
          auto callee = ast.children[0];
          while (file.node(callee).kind == AstKind::NeutralBracket ||
                 file.node(callee).kind == AstKind::Parenthesized)
            callee = file.node(callee).children[0];
          auto ref = n.source.node;
          ref.node = callee;
          if (c->direct_symbol)
            for (const auto &o : m.resolved->occurrences)
              if (same_ref(o.source, ref) &&
                  symbol(o.symbol).kind == modules::SymbolKind::function) {
                auto target = *c->direct_symbol;
                for (const auto &instance : m.instances)
                  if (instance.symbol == target) {
                    target = instance.origin;
                    break;
                  }
                need(target == o.symbol,
                     "owning call target differs from resolved source");
              }
        }
      }
      if (auto b = std::get_if<Binding>(&n.data)) {
        const auto &a = file.node(n.source.node.node);
        if (a.children.size() == 2) {
          auto ref = n.source.node;
          ref.node = a.children[0];
          auto expected = source_type(ref);
          for (const auto &instance : m.instances)
            if (instance.symbol == scope(n.scope).function) {
              const auto &t = *templates.at(instance.origin.value);
              Substitutions sub;
              for (size_t j = 0; j < t.params.size(); ++j)
                sub.push_back({t.params[j].symbol, instance.type_args[j]});
              expected = substitute(expected, sub, substituted);
            }
          need(expected == node(n.children[0]).type,
               "binding annotation type differs from source");
        }
        // A function-valued local const keeps either its direct function
        // Source or a real Read of another local function const. Bind the
        // initializer child to the actual Binding RHS, allowing only the same
        // transparent wrappers as other source_input checks.
        if (!a.children.empty() &&
            file.tokens.at(a.token_begin).spelling == "const" &&
            type(node(n.children[0]).type).kind == TypeKind::Function) {
          source_input(n.children[0], a.children.back(),
                       "const function initializer differs from real source "
                       "initializer");
          auto rhs = n.source.node;
          rhs.node = a.children.back();
          auto target = resolved_source(rhs);
          if (target && symbol(*target).kind == modules::SymbolKind::constant &&
              locals.count(target->value)) {
            auto *initializer =
                std::get_if<Read>(&node(n.children[0]).data);
            need(initializer,
                 "local function constant alias did not preserve a real Read");
            need(origin(initializer->symbol) == *target,
                 "local function constant alias read differs from its source "
                 "binding");
          }
        }
      }
      if (n.kind() == NodeKind::index_read ||
          n.kind() == NodeKind::index_place) {
        need(ast.kind == AstKind::NeutralBracket && ast.children.size() == 2,
             "index source kind/arity");
        auto index = file.node(ast.children[1]);
        if (index.kind == AstKind::BracketItem)
          index = file.node(index.children[0]);
        need(node(n.children[0]).source.span.begin_byte >=
                     file.node(ast.children[0]).span.begin_byte &&
                 node(n.children[0]).source.span.end_byte <=
                     file.node(ast.children[0]).span.end_byte &&
                 node(n.children[1]).source.span.begin_byte >=
                     index.span.begin_byte &&
                 node(n.children[1]).source.span.end_byte <=
                     index.span.end_byte,
             "index source evaluation order");
      }
      if (auto sl = std::get_if<VecSlice>(&n.data)) {
        need(ast.kind == AstKind::Slice, "slice source kind");
        bool start = ast.children.size() > 1 &&
                     file.node(ast.children[1]).kind == AstKind::BracketItem;
        need(bool(sl->start) == start &&
                 bool(sl->end) == (ast.children.size() == 3 ||
                                   (ast.children.size() == 2 && !start)),
             "slice omitted endpoint differs from tokens");
        source_input(n.children[0], ast.children[0],
                     "slice receiver/endpoint differs from source evaluation order");
        if (sl->start)
          source_input(*sl->start, ast.children[1],
                       "slice receiver/endpoint differs from source evaluation order");
        if (sl->end)
          source_input(*sl->end, ast.children.back(),
                       "slice receiver/endpoint differs from source evaluation order");
      }
      if (n.kind() == NodeKind::vec_init) {
        need(ast.kind == AstKind::List &&
                 ast.children.size() == n.children.size(),
             "list source kind/arity");
        for (size_t j = 0; j < n.children.size(); ++j) {
          const auto &a = file.node(ast.children[j]);
          const auto &v = node(n.children[j]);
          need(v.source.span.begin_byte >= a.span.begin_byte &&
                   v.source.span.end_byte <= a.span.end_byte,
               "list source order");
        }
      }
    }
  }

  // Reconstruct effects from types and the structured tree, never from a
  // checker's effect summaries or a published/verified token.
  void effects_and_exits() const {
    std::vector<std::set<uint64_t>> errors(m.nodes.size());
    std::vector<size_t> postorder(m.nodes.size());
    for (size_t i = 0; i < postorder.size(); ++i)
      postorder[i] = i;
    std::sort(postorder.begin(), postorder.end(),
              [&](auto a, auto b) { return leave[a] < leave[b]; });
    auto own_error = [&](const HirNode &n) -> std::optional<TypeId> {
      if (auto c = std::get_if<Call>(&n.data))
        return type(c->signature).throws_type;
      if (auto c = std::get_if<ConstraintCall>(&n.data))
        return type(c->signature).throws_type;
      if (auto r = std::get_if<ForeignRead>(&n.data))
        return m.foreign_bindings.at(r->binding.value).read_error;
      if (n.kind() == NodeKind::throw_)
        return node(n.children[0]).type;
      if (n.kind() == NodeKind::await_) {
        const auto &task = type(node(n.children[0]).type);
        if (task.kind != TypeKind::Task || task.type_args.size() != 2)
          throw std::invalid_argument("await child is not a Task");
        const auto error = task.type_args.at(1);
        if (type(error).kind != TypeKind::Unit)
          return error;
      }
      return {};
    };
    for (auto id : postorder) {
      check_cancelled(stop);
      const auto &n = m.nodes[id];
      auto &out = errors[id];
      if (auto t = std::get_if<Try>(&n.data)) {
        out = errors[n.children[0].value];
        for (const auto &c : t->catches)
          out.erase(c.error_type.value);
        for (const auto &c : t->catches) {
          const auto &handler = errors[c.body.value];
          out.insert(handler.begin(), handler.end());
        }
      } else if (n.kind() == NodeKind::defer_) {
        need(errors[n.children[0].value].empty(),
             "typed error escapes cleanup body");
      } else {
        for (auto ch : n.children)
          out.insert(errors[ch.value].begin(), errors[ch.value].end());
        if (auto e = own_error(n))
          out.insert(e->value);
      }
      std::vector<TypeId> expected;
      for (auto e : out)
        expected.push_back(TypeId{e});
      need(n.errors == expected,
           "effect set differs from independently computed errors");
    }
    std::map<uint64_t, std::set<uint64_t>> references;
    auto contains = [&](HirNodeId outer, HirNodeId inner) {
      return enter[outer.value] <= enter[inner.value] &&
             leave[inner.value] <= leave[outer.value];
    };
    for (size_t i = 0; i < m.nodes.size(); ++i) {
      check_cancelled(stop);
      HirNodeId id{i};
      const auto &n = node(id);
      auto owner = scope(n.scope).function;
      std::optional<SymbolId> read;
      if (auto r = std::get_if<Read>(&n.data))
        read = r->symbol;
      if (auto p = std::get_if<Place>(&n.data))
        read = p->symbol;
      if (auto p = std::get_if<Move>(&n.data))
        read = p->binding;
      std::optional<HirNodeId> enclosing_defer;
      auto branch = id;
      // Ancestor inspection also distinguishes the protected body of a Try
      // from its handlers. A handler cannot catch itself or a sibling.
      std::optional<CatchTarget> handler;
      const auto error = own_error(n);
      for (auto parent = parents[i]; parent; parent = parents[parent->value]) {
        const auto &p = node(*parent);
        if (p.kind() == NodeKind::defer_) {
          if (!enclosing_defer)
            enclosing_defer = *parent;
          if (read) {
            const auto &l = locals.at(read->value);
            if (!ancestor(node(p.children[0]).scope, l.scope)) {
              need(l.function == owner && ancestor(l.scope, p.scope) &&
                       l.ready < enter[parent->value],
                   "defer reference was not visible at registration");
              references[parent->value].insert(read->value);
            }
          }
        }
        if (error && !handler && !enclosing_defer &&
            p.kind() == NodeKind::try_ && branch == p.children[0]) {
          const auto &cs = std::get<Try>(p.data).catches;
          for (size_t c = 0; c < cs.size(); ++c)
            if (cs[c].error_type == *error) {
              handler = CatchTarget{*parent, uint32_t(c)};
              break;
            }
        }
        branch = *parent;
      }
      std::vector<ExitPlan> expected;
      auto exit = [&](ExitKind kind, ExitTarget target,
                      std::optional<ScopeId> keep, std::optional<TypeId> e,
                      bool saves) {
        ExitPlan plan{kind, e, target, {}, {}, saves};
        std::optional<ScopeId> s = n.scope;
        while (s != keep) {
          need(s.has_value(), "exit target is not an ancestor scope");
          plan.exited_scopes.push_back(*s);
          const auto construct = scope_nodes.at(s->value);
          need(construct.has_value(), "exit scope has no owning construct");
          const auto &block = node(*construct);
          if (block.kind() == NodeKind::block) {
            size_t before = block.children.size();
            if (*construct != id) {
              before = 0;
              while (before < block.children.size() &&
                     !contains(block.children[before], id))
                ++before;
              need(before < block.children.size(),
                   "exit is outside its scope's block");
            }
            while (before) {
              auto candidate = block.children[--before];
              if (node(candidate).kind() == NodeKind::defer_)
                plan.defers.push_back(candidate);
            }
          }
          s = scope(*s).parent;
        }
        if (e || !plan.defers.empty())
          expected.push_back(std::move(plan));
      };
      if (error) {
        if (handler)
          exit(ExitKind::error, *handler, node(handler->try_node).scope, error,
               true);
        else {
          need(!enclosing_defer, "error crosses defer boundary");
          const auto &fsig = type(function(owner).signature);
          std::optional<TypeId> declared = fsig.throws_type;
          if (!declared && function(owner).async) {
            const auto &task = type(fsig.result);
            if (task.kind == TypeKind::Task && task.type_args.size() == 2) {
              const auto task_error = task.type_args.at(1);
              if (type(task_error).kind != TypeKind::Unit)
                declared = task_error;
            }
          }
          need(declared == error,
               "unhandled typed error not declared by function");
          exit(ExitKind::error, FunctionExit{owner}, {}, error, true);
        }
      }
      if (n.kind() == NodeKind::return_) {
        need(!enclosing_defer, "return crosses defer boundary");
        exit(ExitKind::return_, FunctionExit{owner}, {}, {},
             !n.children.empty());
      }
      if (n.kind() == NodeKind::await_) {
        need(!enclosing_defer, "await crosses defer boundary");
        exit(ExitKind::cancel, FunctionExit{owner}, {}, {}, false);
      }
      if (n.kind() == NodeKind::break_ || n.kind() == NodeKind::continue_) {
        const auto target = n.kind() == NodeKind::break_
                                ? std::get<Break>(n.data).loop
                                : std::get<Continue>(n.data).loop;
        if (enclosing_defer)
          need(contains(node(*enclosing_defer).children[0], target),
               "jump crosses defer boundary");
        std::optional<ScopeId> keep = node(target).scope;
        if (n.kind() == NodeKind::break_ &&
            (node(target).kind() == NodeKind::for_ ||
             node(target).kind() == NodeKind::for_in))
          keep = scope(*keep).parent;
        exit(n.kind() == NodeKind::break_ ? ExitKind::break_
                                          : ExitKind::continue_,
             JumpExit{target}, keep, {}, false);
      }
      if (n.kind() == NodeKind::block || n.kind() == NodeKind::for_ ||
          n.kind() == NodeKind::for_in)
        exit(ExitKind::normal, ScopeExit{scope(n.scope).parent},
             scope(n.scope).parent, {}, false);
      auto actual = n.exits;
      for (auto &p : actual)
        p.actions.clear();
      std::erase_if(actual, [](const auto &p) {
        return !p.error_type && p.defers.empty();
      });
      need(actual == expected,
           "exit target/scopes/cleanup order/value preservation mismatch");
      if (auto d = std::get_if<Defer>(&n.data))
        need(scope_nodes.at(d->owner_scope.value) &&
                 node(*scope_nodes.at(d->owner_scope.value)).kind() ==
                     NodeKind::block,
             "defer registration must belong to nearest lexical block");
    }
    for (size_t i = 0; i < m.nodes.size(); ++i)
      if (auto d = std::get_if<Defer>(&m.nodes[i].data)) {
        std::vector<SymbolId> expected;
        for (auto s : references[i])
          expected.push_back(SymbolId{s});
        need(d->references == expected,
             "defer references differ from actual external uses");
      }
  }

  void uses_and_flow() const {
    for (size_t i = 0; i < m.nodes.size(); ++i) {
      check_cancelled(stop);
      const auto &n = m.nodes[i];
      if (n.kind() != NodeKind::read && n.kind() != NodeKind::place &&
          !(n.kind() == NodeKind::move && std::get<Move>(n.data).binding))
        continue;
      auto s = n.kind() == NodeKind::read    ? std::get<Read>(n.data).symbol
               : n.kind() == NodeKind::place ? std::get<Place>(n.data).symbol
                                             : *std::get<Move>(n.data).binding;
      symbol(s);
      auto it = locals.find(s.value);
      need(it != locals.end(), "read/place has no typed binding");
      const auto &l = it->second;
      need(owners[i] && *owners[i] == l.function &&
               ancestor(l.scope, n.scope) && l.ready < enter[i],
           "binding out of scope or used before initialization");
      need(n.type == l.type && (n.kind() != NodeKind::place || l.writable),
           "binding type/writability mismatch");
      need(!(l.read_only && n.kind() == NodeKind::move),
           "read-only capture cannot be moved");
    }
    // Postorder without recursive C++ calls. The traversal already rejected
    // DAGs/cycles.
    std::vector<size_t> order(m.nodes.size());
    for (size_t i = 0; i < order.size(); ++i)
      order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return leave[a] < leave[b]; });
    // Recompute reachable typed exits too. Stored HirNode.errors are not used
    // to decide whether a handler can complete normally or break a loop.
    struct Flow {
      bool falls = true;
      std::set<uint64_t> errors, breaks, continues;
    };
    std::vector<Flow> flows(m.nodes.size());
    for (auto i : order) {
      check_cancelled(stop);
      const auto &n = m.nodes[i];
      auto &out = flows[i];
      auto child = [&](size_t index) -> const Flow & {
        return flows[n.children[index].value];
      };
      auto merge = [&](const Flow &f) {
        out.errors.insert(f.errors.begin(), f.errors.end());
        out.breaks.insert(f.breaks.begin(), f.breaks.end());
        out.continues.insert(f.continues.begin(), f.continues.end());
      };
      switch (n.kind()) {
      case NodeKind::defer_:
        break;
      case NodeKind::short_circuit: {
        merge(child(0));
        const auto *known = std::get_if<BoolLiteral>(&node(n.children[0]).data);
        const auto op = std::get<ShortCircuit>(n.data).op;
        if (!known || (op == LogicalOp::and_ ? known->value : !known->value))
          merge(child(1));
        break;
      }
      case NodeKind::block:
        for (auto c : n.children) {
          if (!out.falls)
            break;
          merge(flows[c.value]);
          out.falls = flows[c.value].falls;
        }
        break;
      case NodeKind::if_: {
        merge(child(0));
        const auto *condition =
            std::get_if<BoolLiteral>(&node(n.children[0]).data);
        out.falls = n.children.size() == 2 && (!condition || !condition->value);
        for (size_t branch = 1; branch < n.children.size(); ++branch) {
          if (condition && branch != (condition->value ? 1u : 2u))
            continue;
          merge(child(branch));
          out.falls |= child(branch).falls;
        }
        break;
      }
      case NodeKind::try_: {
        const auto &body = child(0);
        out = body;
        const auto &handlers = std::get<Try>(n.data).catches;
        for (const auto &c : handlers)
          out.errors.erase(c.error_type.value);
        for (const auto &c : handlers) {
          bool reachable = std::any_of(
              body.errors.begin(), body.errors.end(), [&](auto error) {
                return type_patterns_overlap(TypeId{error}, c.error_type,
                                             m.types);
              });
          if (!reachable)
            continue;
          const auto &handler = flows[c.body.value];
          merge(handler);
          out.falls |= handler.falls;
        }
        break;
      }
      case NodeKind::switch_:
        merge(child(0));
        out.falls = false;
        for (size_t arm = 1; arm < n.children.size(); ++arm) {
          const auto &f = child(arm);
          merge(f);
          out.falls |= f.falls || f.breaks.contains(i);
        }
        out.breaks.erase(i);
        break;
      case NodeKind::for_in:
        merge(child(0));
        merge(child(1));
        out.breaks.erase(i);
        out.continues.erase(i);
        out.falls = true;
        break;
      case NodeKind::for_: {
        merge(child(0));
        merge(child(1));
        const auto *condition =
            std::get_if<BoolLiteral>(&node(n.children[1]).data);
        if (condition && !condition->value)
          break;
        const auto &body = child(3);
        merge(body);
        if (body.falls || body.continues.contains(i))
          merge(child(2));
        out.falls = !condition || body.breaks.contains(i);
        out.breaks.erase(i);
        out.continues.erase(i);
        break;
      }
      default:
        for (auto c : n.children)
          merge(flows[c.value]);
        if (auto c = std::get_if<Call>(&n.data))
          if (auto e = type(c->signature).throws_type)
            out.errors.insert(e->value);
        if (auto c = std::get_if<ConstraintCall>(&n.data))
          if (auto e = type(c->signature).throws_type)
            out.errors.insert(e->value);
        if (auto r = std::get_if<ForeignRead>(&n.data))
          out.errors.insert(m.foreign_bindings.at(r->binding.value).read_error.value);
        if (n.kind() == NodeKind::throw_) {
          out.errors.insert(node(n.children[0]).type.value);
          out.falls = false;
        }
        if (n.kind() == NodeKind::return_)
          out.falls = false;
        if (auto b = std::get_if<Break>(&n.data)) {
          out.breaks.insert(b->loop.value);
          out.falls = false;
        }
        if (auto c = std::get_if<Continue>(&n.data)) {
          out.continues.insert(c->loop.value);
          out.falls = false;
        }
        break;
      }
    }
    for (const auto *fp : all_functions) {
      TypeId result = type(fp->signature).result;
      if (fp->async) {
        const auto &task = type(result);
        need(task.kind == TypeKind::Task && task.type_args.size() == 2,
             "async function result is not a Task");
        result = task.type_args.at(0);
      }
      need(type(result).kind == TypeKind::Unit ||
               !flows[fp->body.value].falls,
           "non-Unit function has a normal exit without return");
    }
  }

public:
  Verifier(const HirModule &module, std::stop_token token)
      : m(module), stop(token), enter(m.nodes.size()), leave(m.nodes.size()),
        owners(m.nodes.size()), scope_used(m.scopes.size()),
        parents(m.nodes.size()), scope_nodes(m.scopes.size()),
        substituted(m.types) {}
  void run() {
    check_cancelled(stop);
    need(m.config.backend == Backend::native ||
             m.config.backend == Backend::python,
         "invalid backend");
    need(m.config.word_bits == 32 || m.config.word_bits == 64,
         "invalid target word width");
    need(m.resolved && m.resolved->project,
         "missing owning ResolvedProject/Project");
    const auto &project = *m.resolved->project;
    need(project.word_bits == m.config.word_bits &&
             project.backend ==
                 (m.config.backend == Backend::native ? "native" : "python"),
         "HIR target differs from project target");
    need(m.identity.value < project.modules.size() &&
             project.modules[m.identity.value].id == m.identity,
         "module identity index");
    auto projected = m.types;
    auto expected_bindings = detail::binding_records(project, projected);
    need(projected.size() == m.types.size() && expected_bindings == m.foreign_bindings,
         "foreign typed records differ from declarations");
    declarations();
    std::set<uint64_t> declared_parameters;
    for (const auto &sym : m.resolved->symbols)
      if (sym.kind == modules::SymbolKind::type_parameter) {
        bool real = sym.definition.has_value();
        for (auto [ref, self] : m.resolved->self_parameters)
          if (self == sym.id) {
            source({ref, m.resolved->project->module(ref.module)
                             .files.at(ref.file)
                             ->node(ref.node)
                             .span});
            real = true;
          }
        need(real, "type parameter lacks a real declaration or Self owner");
        declared_parameters.insert(sym.id.value);
      }
    for (size_t i = 1; i <= m.types.size(); ++i) {
      valid_type(TypeId{i}, declared_parameters);
      auto t = type(TypeId{i});
      if (t.kind == TypeKind::NominalStruct || t.kind == TypeKind::NominalEnum)
        constraints(nominal_type(TypeId{i}).params, t.type_args);
    }
    for (const auto &f : m.functions)
      all_functions.push_back(&f);
    for (const auto &t : m.templates) {
      need(templates.emplace(t.function.symbol.value, &t).second,
           "duplicate template identity");
      all_functions.push_back(&t.function);
    }
    for (const auto *fp : all_functions) {
      const auto &f = *fp;
      check_cancelled(stop);
      source(f.source);
      auto ast_kind = project.modules[f.source.node.module.value]
                          .files[f.source.node.file]
                          ->node(f.source.node.node)
                          .kind;
      need(ast_kind == AstKind::Function ||
               ast_kind == AstKind::FunctionLiteral,
           "function source is not a declaration or literal");
      declaration(f.symbol, f.source, modules::SymbolKind::function, ast_kind);
      need(functions.emplace(f.symbol.value, &f).second,
           "duplicate function identity");
      const auto &sig = type(f.signature);
      need(sig.kind == TypeKind::Function, "function signature kind");
      valid_type(f.signature, allowed(f.symbol));
      if (templates.count(f.symbol.value) || source_generics(f.source).empty())
        need(f.signature == source_signature(f.source.node),
             "function signature differs from real source declaration");
      need(sig.parameters.size() == f.parameters.size(),
           "function parameter count");
      if (f.async)
        for (const auto &parameter : sig.parameters)
          need(parameter.mode != ParamMode::inout,
               "async function retains inout parameter storage");
      need(scope(f.scope).function == f.symbol && !scope(f.scope).parent,
           "function scope ownership");
      std::set<std::string> labels;
      for (size_t i = 0; i < f.parameters.size(); ++i) {
        const auto &p = f.parameters[i];
        const auto &s = symbol(p.symbol);
        need(s.kind == modules::SymbolKind::value && s.definition &&
                 s.declaration,
             "parameter symbol kind/source");
        Source src{*s.definition, *s.declaration};
        declaration(p.symbol, src, modules::SymbolKind::value,
                    AstKind::Parameter);
        const auto &file =
            *project.modules[src.node.module.value].files[src.node.file];
        const auto &ast = file.node(src.node.node);
        need(ast.token_begin + 1 < file.tokens.size(), "parameter token range");
        auto mode_token = file.tokens[ast.token_begin + 1].spelling;
        auto declared_mode = mode_token == "inout"  ? ParamMode::inout
                             : mode_token == "move" ? ParamMode::move
                                                    : ParamMode::ordinary;
        need(sig.parameters[i].mode == declared_mode,
             "parameter mode differs from source declaration");
        need(src.node.module == f.source.node.module &&
                 src.node.file == f.source.node.file &&
                 src.span.begin_byte >= f.source.span.begin_byte &&
                 src.span.end_byte <= f.source.span.end_byte,
             "parameter belongs to another function");
        need((s.name == "_" && p.label.empty()) ||
                 (s.name != "_" && !p.label.empty() && p.label == s.name &&
                  labels.insert(p.label).second),
             "duplicate/invalid parameter label");
        need(locals
                 .emplace(p.symbol.value,
                          Local{sig.parameters[i].type, f.scope, f.symbol,
                                declared_mode == ParamMode::inout, 0})
                 .second,
             "duplicate parameter symbol");
      }
      if (ast_kind == AstKind::FunctionLiteral) {
        const auto expected = capture_origins(f.source.node);
        need(expected.size() == f.captures.size(),
             "HirFunction capture count/order differs from source free "
             "variables");
        for (size_t i = 0; i < expected.size(); ++i)
          need(f.captures[i].origin == expected[i],
               "HirFunction capture origin/order differs from source free "
               "variables");
      } else
        need(f.captures.empty(),
             "ordinary function cannot declare capture bindings");
      if (!f.captures.empty()) {
        std::set<uint64_t> origins, locals_seen;
        for (const auto &capture : f.captures) {
          const auto &origin = symbol(capture.origin);
          need(origin.kind == modules::SymbolKind::value &&
                   origins.insert(capture.origin.value).second,
               "capture origin is not a unique value binding");
          const auto &local = symbol(capture.local);
          need(local.kind == modules::SymbolKind::value &&
                   !local.definition && !local.declaration &&
                   locals_seen.insert(capture.local.value).second,
               "capture local is not a fresh definitionless value binding");
          valid_type(capture.type, allowed(f.symbol));
          need(locals
                   .emplace(capture.local.value,
                            Local{capture.type, f.scope, f.symbol, false, 0,
                                  true})
                   .second,
               "duplicate capture local identity");
        }
      }
    }
    // Validate all edges before traversing so bad indices cannot escape via
    // at/UB.
    for (size_t i = 0; i < m.scopes.size(); ++i) {
      check_cancelled(stop);
      const auto &s = m.scopes[i];
      const auto &owner = function(s.function);
      source(s.source);
      need(s.source.node.module == owner.source.node.module &&
               s.source.node.file == owner.source.node.file &&
               s.source.span.begin_byte >= owner.source.span.begin_byte &&
               s.source.span.end_byte <= owner.source.span.end_byte,
           "scope source belongs to another function");
      if (s.parent)
        need(scope(*s.parent).function == s.function && *s.parent != ScopeId{i},
             "scope parent ownership");
      auto current = s.parent;
      for (size_t count = 0; current; ++count) {
        need(count < m.scopes.size(), "scope parent cycle");
        check_cancelled(stop);
        current = scope(*current).parent;
      }
    }
    for (size_t i = 0; i < m.nodes.size(); ++i) {
      scope(m.nodes[i].scope);
      for (auto c : m.nodes[i].children)
        node(c);
      shape(HirNodeId{i});
    }
    for (const auto *f : all_functions) {
      tree(*f);
      source_effect_nodes(*f);
    }
    instance_records();
    for (const auto &n : m.nodes) {
      if (auto c = std::get_if<ConstantLiteral>(&n.data))
        check_constant(n, c->value);
      if (auto b = std::get_if<Binding>(&n.data);
          b && symbol(b->symbol).kind == modules::SymbolKind::constant &&
          aggregate_constant_expression(m, n.children.at(0))) {
        auto value = evaluate_hir_constant(m, n.children[0], stop);
        need(value.has_value(), "aggregate const initializer is not pure");
        check_constant(node(n.children[0]), *value);
      }
    }
    function_value_sources();
    public_constant_values();
    finite_layouts();
    for (auto owner : owners)
      need(owner.has_value(), "orphan executable node");
    for (bool used : scope_used)
      need(used, "orphan scope");
    uses_and_flow();
    ownership_sources();
    for (const auto &local : m.ownership.locals)
      if (local.read_only_capture) {
        bool found = false;
        for (const auto *f : all_functions)
          for (const auto &capture : f->captures)
            if (capture.local == local.symbol &&
                capture.type == local.type)
              found = true;
        need(found, "read-only capture lifetime has no lambda capture entry");
      }
    effects_and_exits();
    auto ownership = detail::verify_ownership(m, stop);
    if (ownership.state == PhaseState::cancelled)
      throw Cancelled{};
    if (!ownership.success())
      fail(ownership.diagnostics.front().message,
           ownership.diagnostics.front().primary);
  }
};
std::shared_ptr<const HirModule> freeze(const HirModule &input,
                                        std::stop_token stop) {
  check_cancelled(stop);
  auto result = std::make_shared<HirModule>(input);
  if (input.resolved) {
    auto resolved = std::make_shared<modules::ResolvedProject>(*input.resolved);
    if (input.resolved->project) {
      auto project =
          std::make_shared<modules::Project>(*input.resolved->project);
      for (auto &mod : project->modules)
        for (auto &file : mod.files) {
          check_cancelled(stop);
          if (file)
            file = std::make_shared<const AstFile>(*file);
        }
      resolved->project = std::move(project);
    }
    result->resolved = std::move(resolved);
  }
  return result;
}
} // namespace
PhaseResult<std::monostate> verify_types(const HirModule &input,
                                         std::stop_token stop) {
  PhaseResult<std::monostate> r;
  try {
    Verifier(input, stop).run();
    r.value = std::monostate{};
    r.state = PhaseState::complete;
  } catch (const Cancelled &) {
    r.state = PhaseState::cancelled;
  } catch (const Invalid &e) {
    r.diagnostics.push_back(
        {"E_IR_INVALID", "lowering", "error", e.message, e.span});
  } catch (const std::exception &e) {
    r.diagnostics.push_back({"E_IR_INVALID", "lowering", "error", e.what()});
  }
  return r;
}
PhaseResult<std::monostate> check_backend_capabilities(const HirModule &m,
                                                       std::stop_token stop) {
  auto r = verify_types(m, stop);
  if (!r.success())
    return r;
  auto unavailable = [&](const Source &s, std::string feature) {
    r.value.reset();
    r.state = PhaseState::failed;
    r.failure = FailureKind::unsupported;
    r.diagnostics.push_back({"E_CAPABILITY_UNAVAILABLE", "type", "error",
                             std::move(feature), s.span});
  };
  try {
    const bool ownership_values =
        (m.config.backend == Backend::native || m.config.backend == Backend::python) &&
        m.config.word_bits == 64;
    const bool foreign_values =
        m.config.backend == Backend::python && m.config.word_bits == 64;
    const bool runtime_handle_values =
        m.config.backend == Backend::native && m.config.word_bits == 64;
    const bool slice_values =
        (m.config.backend == Backend::native ||
         m.config.backend == Backend::python) &&
        m.config.word_bits == 64;
    // Weak handle encodings are defined for the 64-bit profiles only. The
    // source rules are backend-neutral; the emitters are separate owners.
    const bool weak_values =
        (m.config.backend == Backend::native ||
         m.config.backend == Backend::python) &&
        m.config.word_bits == 64;
    const bool c_values =
        m.config.backend == Backend::native && m.config.word_bits == 64;
    auto c_value = [](TypeKind kind) {
      return kind == TypeKind::CForeignPointer ||
             kind == TypeKind::CForeignBuffer ||
             kind == TypeKind::CForeignRecord ||
             kind == TypeKind::CForeignResource ||
             kind == TypeKind::CForeignCallback ||
             kind == TypeKind::CForeignOpaque;
    };
    auto runtime_handle = [](TypeKind kind) {
      return kind == TypeKind::ThreadHandle || kind == TypeKind::MutexHandle ||
             kind == TypeKind::CondvarHandle ||
             kind == TypeKind::AtomicI64Handle || kind == TypeKind::IpcHandle;
    };
    TypeContext storage_types = m.types;
    std::set<uint64_t> checked_types;
    bool type_budget_exceeded = false;
    std::function<bool(TypeId, size_t)> supported = [&](TypeId id,
                                                       size_t depth) {
      check_cancelled(stop);
      if (depth > 512 || checked_types.size() > 65536) {
        type_budget_exceeded = true;
        return false;
      }
      const auto *found = storage_types.find(id);
      if (!found)
        return false;
      const auto t = *found;
      if (runtime_handle(t.kind))
        return runtime_handle_values;
      if (t.kind == TypeKind::ForeignOpaque || t.kind == TypeKind::ForeignCallable)
        return foreign_values;
      if (c_value(t.kind)) {
        if (!c_values) return false;
        for (auto arg : t.type_args)
          if (!storage_types.find(arg) || !supported(arg, depth + 1)) return false;
        return true;
      }
      if (!checked_types.insert(id.value).second)
        return true;
      if (t.kind == TypeKind::Vec || t.kind == TypeKind::Slice ||
          t.kind == TypeKind::Weak) {
        if (t.kind == TypeKind::Slice && !slice_values)
          return false;
        if (t.kind == TypeKind::Weak && !weak_values)
          return false;
        if (!ownership_values || t.type_args.size() != 1)
          return false;
        if (!storage_types.find(t.type_args.at(0)))
          return false;
        return supported(t.type_args.at(0), depth + 1);
      }
      if (t.kind == TypeKind::TraitObject) {
        if (!ownership_values)
          return false;
        for (auto arg : t.type_args)
          if (!storage_types.find(arg) || !supported(arg, depth + 1))
            return false;
        return true;
      }
      if (t.kind == TypeKind::NominalStruct ||
          t.kind == TypeKind::NominalEnum) {
        const auto d = std::find_if(
            m.nominals.begin(), m.nominals.end(),
            [&](const auto &d) { return d.symbol == t.declaration; });
        if (d == m.nominals.end() || d->params.size() != t.type_args.size())
          return false;
        Substitutions subst;
        for (size_t p = 0; p < d->params.size(); ++p)
          subst.emplace_back(d->params[p].symbol, t.type_args[p]);
        auto fields = [&](const auto &fs) {
          for (const auto &f : fs) {
            auto field = substitute(f.type, subst, storage_types);
            // A concrete member must already be public.  Substitution into a
            // private scratch context may not manufacture a missing member.
            if (!m.types.find(field))
              return false;
            if (!storage_types.find(field) ||
                !supported(field, depth + 1))
              return false;
          }
          return true;
        };
        if (!fields(d->fields))
          return false;
        for (const auto &v : d->variants)
          if (!fields(v.payload))
            return false;
        return true;
      }
      if (t.kind == TypeKind::Function) {
        std::vector<TypeId> pending{id};
        std::set<uint64_t> seen;
        while (!pending.empty()) {
          if (pending.size() > 65536) {
            type_budget_exceeded = true;
            return false;
          }
          auto current = pending.back();
          pending.pop_back();
          if (!seen.insert(current.value).second)
            continue;
          const auto *current_found = storage_types.find(current);
          if (!current_found)
            return false;
          const auto value = *current_found;
          if (runtime_handle(value.kind)) {
            if (!runtime_handle_values)
              return false;
            continue;
          }
          if (value.kind == TypeKind::ForeignOpaque ||
              value.kind == TypeKind::ForeignCallable) {
            if (!foreign_values)
              return false;
            continue;
          }
          if (value.kind == TypeKind::Vec || value.kind == TypeKind::Slice ||
              value.kind == TypeKind::Weak) {
            if (value.kind == TypeKind::Slice && !slice_values)
              return false;
            if (value.kind == TypeKind::Weak && !weak_values)
              return false;
            if (!ownership_values || value.type_args.size() != 1)
              return false;
            if (!storage_types.find(value.type_args.at(0)))
              return false;
          }
          if (value.kind == TypeKind::NominalStruct ||
              value.kind == TypeKind::NominalEnum) {
            for (const auto &d : m.nominals)
              if (d.symbol == value.declaration) {
                auto add = [&](TypeId field) {
                  if (!m.types.find(field))
                    return false;
                  if (!storage_types.find(field))
                    return false;
                  pending.push_back(field);
                  return true;
                };
                for (const auto &f : d.fields)
                  if (!add(f.type))
                    return false;
                for (const auto &v : d.variants)
                  for (const auto &f : v.payload)
                    if (!add(f.type))
                      return false;
              }
          }
          pending.insert(pending.end(), value.type_args.begin(),
                         value.type_args.end());
          if (value.kind == TypeKind::Function) {
            pending.push_back(value.result);
            if (value.throws_type)
              pending.push_back(*value.throws_type);
            for (auto p : value.parameters) {
              if (p.mode == ParamMode::move && !ownership_values)
                return false;
              pending.push_back(p.type);
            }
          }
        }
      }
      // Function fields are storage leaves. Do not expand their signatures
      // here (e.g. S[T].f: func() S[S[T]]); functions/calls are checked below.
      return t.kind != TypeKind::Vec && t.kind != TypeKind::Slice &&
             t.kind != TypeKind::TypeParameter &&
             t.kind != TypeKind::Error && t.kind != TypeKind::ForeignOpaque &&
             t.kind != TypeKind::ForeignCallable && !c_value(t.kind);
    };
    auto signature_supported = [&](TypeId id) {
      const auto t = *m.types.find(id);
      if (t.throws_type && !supported(*t.throws_type, 0))
        return false;
      for (auto p : t.parameters)
        if ((p.mode == ParamMode::move && !ownership_values) ||
            !supported(p.type, 0))
          return false;
      return supported(t.result, 0);
    };
    for (const auto &f : m.functions) {
      check_cancelled(stop);
      if (!signature_supported(f.signature)) {
        unavailable(
            f.source,
            type_budget_exceeded
                ? "backend concrete type expansion budget exceeded"
                : "backend does not support this concrete function signature");
        return r;
      }
      std::vector<HirNodeId> todo{f.body};
      while (!todo.empty()) {
        check_cancelled(stop);
        auto id = todo.back();
        todo.pop_back();
        const auto &n = m.nodes[id.value];
        if (const auto *call = std::get_if<Call>(&n.data);
            call && call->kind == CalleeKind::system &&
            (m.config.backend != Backend::native || m.config.word_bits != 64) &&
            std::none_of(value_system_operations.begin(), value_system_operations.end(),
                         [&](const auto &spec) { return spec.operation == *call->system; }) &&
            !(slice_values &&
              std::any_of(slice_system_operations.begin(), slice_system_operations.end(),
                          [&](const auto &spec) { return spec.operation == *call->system; })) &&
            !(weak_values &&
              std::any_of(weak_system_operations.begin(), weak_system_operations.end(),
                          [&](const auto &spec) { return spec.operation == *call->system; }))) {
          unavailable(n.source, "system operations require the Native/64 profile");
          return r;
        }
        if (n.kind() == NodeKind::foreign_read ||
            (std::holds_alternative<Call>(n.data) &&
             std::get<Call>(n.data).kind == CalleeKind::foreign)) {
          if (!foreign_values) {
            unavailable(n.source,
                        "foreign reads/calls/access require the Python/64 profile");
            return r;
          }
          auto binding = n.kind() == NodeKind::foreign_read
                             ? std::get<ForeignRead>(n.data).binding
                             : *std::get<Call>(n.data).binding;
          if (binding.value >= m.foreign_bindings.size()) {
            unavailable(n.source, "foreign binding index is invalid");
            return r;
          }
          const auto &declaration =
              m.resolved->project->foreign_binding(binding);
          if (const auto *callable =
                  std::get_if<modules::BindingCallable>(&declaration.detail)) {
            if (!callable->error_type ||
                *callable->error_type != "lang.py.Error") {
              unavailable(n.source,
                          "Python foreign callable requires declared "
                          "lang.py.Error");
              return r;
            }
            for (const auto &parameter : callable->parameters)
              if (parameter.mode != "ordinary") {
                unavailable(n.source,
                            "Python foreign callable supports ordinary "
                            "parameters only");
                return r;
              }
          }
          if (const auto *property =
                  std::get_if<modules::BindingProperty>(&declaration.detail);
              property && property->error_type != "lang.py.Error") {
            unavailable(n.source, "Python foreign property requires lang.py.Error");
            return r;
          }
          if (const auto *index =
                  std::get_if<modules::BindingIndexer>(&declaration.detail);
              index && index->error_type != "lang.py.Error") {
            unavailable(n.source, "Python foreign indexer requires lang.py.Error");
            return r;
          }
          if (const auto *slice =
                  std::get_if<modules::BindingSlice>(&declaration.detail);
              slice && slice->error_type != "lang.py.Error") {
            unavailable(n.source, "Python foreign slice requires lang.py.Error");
            return r;
          }
        }
        switch (n.kind()) {
        case NodeKind::struct_init:
        case NodeKind::enum_init:
        case NodeKind::field_read:
        case NodeKind::field_place:
        case NodeKind::switch_:
        case NodeKind::trait_object_init:
        case NodeKind::trait_dispatch:
          if (n.kind() == NodeKind::trait_object_init ||
              n.kind() == NodeKind::trait_dispatch)
            if (!ownership_values) {
              unavailable(n.source,
                          "backend has not implemented dyn trait objects");
              return r;
            }
          break;
        case NodeKind::try_:
        case NodeKind::defer_:
        case NodeKind::throw_:
          break;
        case NodeKind::move:
        case NodeKind::vec_init:
        case NodeKind::index_read:
        case NodeKind::index_place:
        case NodeKind::vec_slice:
        case NodeKind::for_in:
          if (ownership_values)
            break;
          [[fallthrough]];
        case NodeKind::constraint_call:
          unavailable(n.source,
                      "backend has not implemented this aggregate operation");
          return r;
        default:
          break;
        }
        if (auto c = std::get_if<Call>(&n.data);
            c && !signature_supported(c->signature)) {
          unavailable(n.source, "backend does not support this call signature");
          return r;
        }
        if (!supported(n.type, 0)) {
          unavailable(n.source,
                      type_budget_exceeded
                          ? "backend concrete type expansion budget exceeded"
                          : "backend does not support this concrete aggregate type");
          return r;
        }
        todo.insert(todo.end(), n.children.begin(), n.children.end());
      }
    }
  } catch (const Cancelled &) {
    r.value.reset();
    r.state = PhaseState::cancelled;
  }
  return r;
}
PhaseResult<VerifiedHirModule> verify(const HirModule &input,
                                      std::stop_token stop, VerifyMode mode) {
  PhaseResult<VerifiedHirModule> r;
  try {
    auto module = freeze(input, stop);
    auto checked = mode == VerifyMode::backend
                       ? check_backend_capabilities(*module, stop)
                       : verify_types(*module, stop);
    if (checked.success()) {
      const auto &p = *module->resolved->project;
      for (const auto &mod : p.modules)
        if (mod.binding) {
          const auto &backends = p.bindings.at(mod.binding->bundle).allowed_backends;
          const std::string name = module->config.backend == Backend::python ? "python" : "native";
          if (std::find(backends.begin(), backends.end(), name) == backends.end()) {
            checked.value.reset(); checked.state = PhaseState::failed;
            checked.failure = FailureKind::unsupported;
            checked.diagnostics.push_back({"E_CAPABILITY_UNAVAILABLE", "type", "error",
              "foreign source module is unavailable for this backend"});
            break;
          }
        }
    }
    if (!checked.success()) {
      r.state = checked.state;
      r.failure = checked.failure;
      r.diagnostics = std::move(checked.diagnostics);
      return r;
    }
    check_cancelled(stop);
    r.value = VerifiedHirModule(std::move(module));
    r.state = PhaseState::complete;
  } catch (const Cancelled &) {
    r.state = PhaseState::cancelled;
  } catch (const Invalid &e) {
    Diagnostic d;
    d.code = "E_IR_INVALID";
    d.phase = "lowering";
    d.severity = "error";
    d.message = e.message;
    d.primary = e.span;
    r.diagnostics.push_back(std::move(d));
  }
  return r;
}
namespace {
void print_source(std::ostream &s, const Source &src) {
  s << " src=" << src.node.module.value << ':' << src.node.file << ':'
    << src.node.node.value << '@' << src.span.snapshot.value << ':'
    << src.span.begin_byte << ':' << src.span.end_byte;
}
} // namespace
std::string dump(const TypeContext &c) {
  std::ostringstream s;
  s.imbue(std::locale::classic());
  for (size_t i = 1; i <= c.size(); ++i) {
    const auto &t = *c.find(TypeId{i});
    s << "type " << i << " kind=" << int(t.kind) << " bits=" << t.bits
      << " declaration=" << t.declaration.value
      << " binding=" << t.binding.value << " result=" << t.result.value
      << " throws=";
    if (t.throws_type)
      s << t.throws_type->value;
    else
      s << '-';
    for (auto p : t.parameters)
      s << " p=" << int(p.mode) << ':' << p.type.value;
    for (auto a : t.type_args)
      s << " arg=" << a.value;
    s << '\n';
  }
  return s.str();
}
std::string dump(const HirModule &m) {
  std::ostringstream s;
  s.imbue(std::locale::classic());
  s << contract_version << " scalar-v1 module=" << m.identity.value
    << " backend=" << int(m.config.backend)
    << " word_bits=" << m.config.word_bits << '\n';
  s << dump(m.types);
  for (const auto &b : m.foreign_bindings) {
    s << "foreign " << b.binding.value << " type=" << b.type.value << " signature=";
    if (b.signature) s << b.signature->value; else s << '-';
    s << " owner=";
    if (b.owner) s << b.owner->value; else s << '-';
    s << " read_error=" << b.read_error.value << '\n';
  }
  auto trait_ref = [&](const TraitRef &r) {
    s << r.declaration.value << '[';
    for (size_t i = 0; i < r.args.size(); ++i) {
      if (i)
        s << ',';
      s << r.args[i].value;
    }
    s << ']';
  };
  auto params = [&](const auto &ps) {
    for (const auto &p : ps) {
      s << " generic=" << p.symbol.value << ':';
      if (p.trait)
        trait_ref(*p.trait);
      else
        s << '-';
    }
  };
  auto field = [&](const FieldDecl &f) {
    s << " field=" << f.symbol.value << ':' << f.type.value << ':'
      << f.exported;
  };
  for (const auto &d : m.nominals) {
    s << "nominal " << d.symbol.value << " kind=" << int(d.kind);
    params(d.params);
    print_source(s, d.source);
    for (const auto &f : d.fields)
      field(f);
    s << '\n';
    for (const auto &v : d.variants) {
      s << "variant " << v.symbol.value << " owner=" << d.symbol.value;
      for (const auto &f : v.payload)
        field(f);
      print_source(s, v.source);
      s << '\n';
    }
  }
  for (const auto &t : m.traits) {
    s << "trait " << t.symbol.value << " self=" << t.self.value;
    params(t.params);
    print_source(s, t.source);
    s << '\n';
    for (const auto &p : t.methods) {
      s << "prototype " << p.symbol.value << " signature=" << p.signature.value;
      for (auto a : p.parameters)
        s << " param=" << a.symbol.value << ':' << std::quoted(a.label);
      print_source(s, p.source);
      s << '\n';
    }
  }
  for (const auto &im : m.impls) {
    s << "impl self=" << im.self_type.value
      << " self_parameter=" << im.self_parameter.value << " trait=";
    if (im.trait)
      trait_ref(*im.trait);
    else
      s << '-';
    params(im.params);
    for (auto method : im.methods)
      s << " method=" << method.value;
    print_source(s, im.source);
    s << '\n';
  }
  for (const auto &t : m.templates) {
    s << "template " << t.function.symbol.value
      << " signature=" << t.function.signature.value
      << " scope=" << t.function.scope.value
      << " body=" << t.function.body.value;
    params(t.params);
    print_source(s, t.function.source);
    for (auto p : t.function.parameters)
      s << " param=" << p.symbol.value << ':' << std::quoted(p.label);
    for (const auto &c : t.function.captures)
      s << " capture=" << c.origin.value << ':' << c.local.value << ':'
        << c.type.value;
    s << '\n';
  }
  for (const auto &i : m.instances) {
    s << "instance " << i.symbol.value << " origin=" << i.origin.value
      << " backend=" << int(i.target.backend)
      << " word_bits=" << i.target.word_bits;
    for (auto t : i.type_args)
      s << " arg=" << t.value;
    for (auto [from, to] : i.renamed)
      s << " renamed=" << from.value << ':' << to.value;
    s << '\n';
  }
  for (const auto &f : m.functions) {
    s << "function " << f.symbol.value << " signature=" << f.signature.value
      << " scope=" << f.scope.value << " body=" << f.body.value;
    print_source(s, f.source);
    for (const auto &p : f.parameters)
      s << " param=" << p.symbol.value << ':' << std::quoted(p.label);
    for (const auto &c : f.captures)
      s << " capture=" << c.origin.value << ':' << c.local.value << ':'
        << c.type.value;
    s << '\n';
  }
  for (size_t i = 0; i < m.scopes.size(); ++i) {
    const auto &scope = m.scopes[i];
    s << "scope " << i << " function=" << scope.function.value << " parent=";
    if (scope.parent)
      s << scope.parent->value;
    else
      s << '-';
    print_source(s, scope.source);
    s << '\n';
  }
  for (size_t i = 0; i < m.nodes.size(); ++i) {
    const auto &n = m.nodes[i];
    s << "node " << i << " kind=" << int(n.kind()) << " type=" << n.type.value
      << " scope=" << n.scope.value;
    print_source(s, n.source);
    s << " children=";
    for (auto c : n.children)
      s << c.value << ',';
    std::visit(
        [&](const auto &data) {
          using T = std::decay_t<decltype(data)>;
          if constexpr (std::is_same_v<T, BoolLiteral>)
            s << " bool=" << data.value;
          else if constexpr (std::is_same_v<T, IntegerLiteral>)
            s << " decimal=" << std::quoted(data.decimal)
              << " bits=" << data.bits;
          else if constexpr (std::is_same_v<T, FloatLiteral>)
            s << " bits=" << data.bits;
          else if constexpr (std::is_same_v<T, StringLiteral>)
            s << " utf8=" << std::quoted(data.utf8);
          else if constexpr (std::is_same_v<T, ConstantLiteral>) {
            std::function<void(const ConstantValue &)> value =
                [&](const auto &v) {
                  s << " const=" << v.type.value << ':' << v.data.index();
                  std::visit(
                      [&](const auto &x) {
                        using V = std::decay_t<decltype(x)>;
                        if constexpr (std::is_same_v<V, ConstantAggregate>) {
                          if (x.variant)
                            s << " variant=" << x.variant->value;
                          s << '[';
                          for (auto &f : x.fields)
                            value(f);
                          s << ']';
                        } else if constexpr (std::is_same_v<V, StringLiteral>)
                          s << ':' << std::quoted(x.utf8);
                        else if constexpr (std::is_same_v<V, IntegerLiteral>)
                          s << ':' << x.decimal << ':' << x.bits;
                        else if constexpr (std::is_same_v<V, FloatLiteral>)
                          s << ':' << x.bits;
                        else if constexpr (std::is_same_v<V, BoolLiteral>)
                          s << ':' << x.value;
                        else if constexpr (std::is_same_v<V, FunctionValue>) {
                          s << ':' << x.symbol.value;
                          for (auto a : x.type_args)
                            s << ':' << a.value;
                          if (!x.captures.empty())
                            s << " captures=" << x.captures.size();
                          if (x.move_capture)
                            s << " move_capture";
                          if (x.bound)
                            s << " bound=" << x.bound->type.value << ':'
                              << int(x.bound->mode);
                        }
                      },
                      v.data);
                };
            value(data.value);
          } else if constexpr (std::is_same_v<T, Read> ||
                               std::is_same_v<T, Place> ||
                               std::is_same_v<T, FunctionValue>) {
            s << " symbol=" << data.symbol.value;
            if constexpr (std::is_same_v<T, FunctionValue>) {
              for (auto t : data.type_args)
                s << " type_arg=" << t.value;
              for (const auto &c : data.captures)
                s << " capture=" << c.origin.value << ':' << c.source.value
                  << ':' << c.type.value << ':' << c.move;
              if (data.move_capture)
                s << " move_capture";
              if (data.bound)
                s << " bound=" << data.bound->type.value << ':'
                  << int(data.bound->mode);
            }
          } else if constexpr (std::is_same_v<T, Binding>)
            s << " symbol=" << data.symbol.value
              << " writable=" << data.writable;
          else if constexpr (std::is_same_v<T, Assign>) {
            s << " compound=";
            if (data.compound)
              s << int(*data.compound);
            else
              s << '-';
          } else if constexpr (std::is_same_v<T, Unary> ||
                               std::is_same_v<T, Binary> ||
                               std::is_same_v<T, ShortCircuit>)
            s << " op=" << int(data.op);
          else if constexpr (std::is_same_v<T, Return>)
            s << " function=" << data.function.value;
          else if constexpr (std::is_same_v<T, Break> ||
                             std::is_same_v<T, Continue>)
            s << " loop=" << data.loop.value;
          else if constexpr (std::is_same_v<T, Unsupported>)
            s << " unsupported=" << std::quoted(data.feature);
          else if constexpr (std::is_same_v<T, FieldRead> ||
                             std::is_same_v<T, FieldPlace>)
            s << " field=" << data.field.value;
          else if constexpr (std::is_same_v<T, StructInit> ||
                             std::is_same_v<T, EnumInit>) {
            auto arguments = [&](const auto &args) {
              for (const auto &a : args)
                s << " arg=" << a.source_index << ':' << a.parameter_index
                  << ':' << a.expression.value;
            };
            if constexpr (std::is_same_v<T, StructInit>)
              arguments(data.fields);
            else {
              s << " variant=" << data.variant.value;
              arguments(data.payload);
            }
          } else if constexpr (std::is_same_v<T, Switch>) {
            s << " exhaustive=" << data.exhaustive;
            std::function<void(const Pattern &)> pattern =
                [&](const Pattern &p) {
                  s << " pattern=" << p.type.value << ':';
                  if (p.variant)
                    s << p.variant->value;
                  else
                    s << '-';
                  s << ':';
                  if (p.binding)
                    s << p.binding->value;
                  else
                    s << '-';
                  s << " literal=";
                  if (!p.literal) {
                    s << '-';
                  } else {
                    const auto *pt = m.types.find(p.type);
                    const unsigned width =
                        pt && (pt->kind == TypeKind::Float ||
                               pt->kind == TypeKind::SignedInt ||
                               pt->kind == TypeKind::UnsignedInt ||
                               pt->kind == TypeKind::Isize ||
                               pt->kind == TypeKind::Usize)
                            ? bit_width(*pt, m.config)
                            : 0;
                    std::visit(
                        [&](const auto &value) {
                          using T = std::decay_t<decltype(value)>;
                          if constexpr (std::is_same_v<T, BoolLiteral>)
                            s << "bool:" << value.value;
                          else if constexpr (std::is_same_v<T, IntegerLiteral>)
                            s << "int:" << std::quoted(value.decimal) << ':'
                              << width << ':' << value.bits;
                          else if constexpr (std::is_same_v<T, FloatLiteral>)
                            s << "float:" << width << ':' << value.bits;
                          else
                            s << "string:" << std::quoted(value.utf8);
                        },
                        *p.literal);
                  }
                  for (const auto &c : p.payload)
                    pattern(c);
                };
            for (const auto &a : data.arms) {
              s << " arm=" << a.scope.value << ':' << a.body.value;
              pattern(a.pattern);
            }
          } else if constexpr (std::is_same_v<T, Try>) {
            for (const auto &c : data.catches) {
              s << " catch=" << c.error_type.value << ':' << c.binding.value
                << ':' << c.scope.value << ':' << c.body.value;
              print_source(s, c.source);
            }
          } else if constexpr (std::is_same_v<T, Move>) {
            s << " moved_binding=";
            if (data.binding)
              s << data.binding->value;
            else
              s << '-';
          } else if constexpr (std::is_same_v<T, VecSlice>) {
            s << " slice_start=";
            if (data.start)
              s << data.start->value;
            else
              s << '-';
            s << " slice_end=";
            if (data.end)
              s << data.end->value;
            else
              s << '-';
          } else if constexpr (std::is_same_v<T, ForIn>) {
            s << " element=" << data.element.value
              << " body_scope=" << data.body_scope.value
              << " body=" << data.body.value;
          } else if constexpr (std::is_same_v<T, Defer>) {
            s << " owner_scope=" << data.owner_scope.value;
            for (auto r : data.references)
              s << " reference=" << r.value;
          } else if constexpr (std::is_same_v<T, ConstraintCall>) {
            s << " trait=";
            trait_ref(data.trait);
            s << " method=" << data.method.value
              << " receiver=" << data.receiver.value
              << " signature=" << data.signature.value;
            for (const auto &a : data.args)
              s << " arg=" << a.source_index << ':' << a.parameter_index << ':'
                << a.expression.value;
          } else if constexpr (std::is_same_v<T, TraitObjectInit>) {
            s << " trait=";
            trait_ref(data.trait);
            s << " concrete=" << data.concrete.value << " methods=";
            for (size_t i = 0; i < data.methods.size(); ++i) {
              s << data.methods[i].value << ':';
              if (i < data.method_args.size())
                for (auto a : data.method_args[i])
                  s << a.value << ',';
              s << ';';
            }
          } else if constexpr (std::is_same_v<T, TraitDispatch>) {
            s << " trait=";
            trait_ref(data.trait);
            s << " method=" << data.method.value
              << " signature=" << data.signature.value
              << " vtable_index=" << data.index;
            for (const auto &a : data.args)
              s << " arg=" << a.source_index << ':' << a.parameter_index
                << ':' << int(a.mode) << ':' << a.expression.value;
          } else if constexpr (std::is_same_v<T, ForeignRead>) {
            s << " foreign_read=" << data.binding.value;
          } else if constexpr (std::is_same_v<T, CFieldRead>) {
            s << " c_record=" << data.c_type << " c_field=" << data.field;
          } else if constexpr (std::is_same_v<T, Call>) {
            for (auto t : data.type_args)
              s << " type_arg=" << t.value;
            if (data.binding) s << " binding=" << data.binding->value;
            if (data.c_callable) s << " c_callable=" << *data.c_callable;
            if (data.system) s << " system=" << int(*data.system);
            for (auto p : data.omitted) s << " omitted=" << p;
            s << " callee_kind=" << int(data.kind)
              << " signature=" << data.signature.value << " direct=";
            if (data.direct_symbol)
              s << data.direct_symbol->value;
            else
              s << '-';
            s << " callee=";
            if (data.callee)
              s << data.callee->value;
            else
              s << '-';
            for (auto &a : data.args) {
              s << " arg=" << a.source_index << ':' << a.parameter_index << ':'
                << int(a.mode) << ':' << a.expression.value << ':';
              if (a.label)
                s << std::quoted(*a.label);
              else
                s << '-';
            }
          }
        },
        n.data);
    for (auto e : n.errors)
      s << " error=" << e.value;
    for (const auto &e : n.exits) {
      s << " exit=" << int(e.kind) << " saves_value=" << e.saves_value;
      if (e.error_type)
        s << " error_type=" << e.error_type->value;
      std::visit(
          [&](const auto &target) {
            using T = std::decay_t<decltype(target)>;
            if constexpr (std::is_same_v<T, ScopeExit>) {
              s << " resume_scope=";
              if (target.scope)
                s << target.scope->value;
              else
                s << '-';
            } else if constexpr (std::is_same_v<T, FunctionExit>)
              s << " function=" << target.function.value;
            else if constexpr (std::is_same_v<T, JumpExit>)
              s << " jump=" << target.node.value;
            else
              s << " handler=" << target.try_node.value << ':' << target.index;
          },
          e.target);
      s << " exited_scopes=";
      for (auto scope : e.exited_scopes)
        s << scope.value << ',';
      s << " defers=";
      for (auto d : e.defers)
        s << d.value << ',';
      for (auto a : e.actions)
        s << " action=" << int(a.kind) << ':' << int(a.owner.kind) << ':'
          << a.owner.identity << ':' << a.type.value;
    }
    s << '\n';
  }
  for (auto u : m.ownership.uses) {
    s << "use " << u.consumer.value << ':' << u.operand << ':' << int(u.kind)
      << ':' << u.type.value << " root=";
    if (u.root)
      s << u.root->value;
    else
      s << '-';
    if (u.destination)
      s << " destination=" << u.destination->value;
    s << '\n';
  }
  for (auto t : m.ownership.temporaries)
    s << "temporary " << t.producer.value << " full=" << t.full_expression.value
      << " type=" << t.type.value << '\n';
  for (auto l : m.ownership.locals) {
    s << "lifetime " << l.symbol.value << " scope=" << l.scope.value
      << " type=" << l.type.value << " order=" << l.order
      << " mode=" << int(l.mode);
    if (l.read_only_capture)
      s << " read_only_capture";
    s << '\n';
  }
  auto actions = [&](const auto &as) {
    for (auto a : as)
      s << " action=" << int(a.kind) << ':' << int(a.owner.kind) << ':'
        << a.owner.identity << ':' << a.type.value;
    s << '\n';
  };
  for (const auto &f : m.ownership.full_expressions) {
    s << "full_expression " << f.node.value;
    actions(f.actions);
  }
  for (const auto &c : m.ownership.calls) {
    s << "call_return " << c.node.value;
    actions(c.on_return);
  }
  return s.str();
}
std::string dump(const VerifiedHirModule &m) { return dump(m.module()); }
} // namespace lang::hir
