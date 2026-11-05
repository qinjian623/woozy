#include "lang/hir/hir.hpp"
#include "ownership.hpp"
#include "foreign.hpp"
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
    if (t.type_args.size() != 1)
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
  case TypeKind::Error:
    break;
  default:
    bad();
  }
  if (t.kind != TypeKind::Function &&
      (!t.parameters.empty() || t.result.value || t.throws_type))
    bad();
  if (t.kind != TypeKind::SignedInt && t.kind != TypeKind::UnsignedInt &&
      t.kind != TypeKind::Float && t.bits)
    bad();
  if (!nominal(t.kind) && t.kind != TypeKind::ForeignOpaque && t.kind != TypeKind::ForeignCallable &&
      (t.declaration.value || t.binding.value))
    bad();
  if (nominal(t.kind) && t.binding.value)
    bad();
  if (t.kind != TypeKind::NominalStruct && t.kind != TypeKind::NominalEnum &&
      t.kind != TypeKind::Vec && !t.type_args.empty())
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
  mutable std::map<std::pair<uint64_t, uint64_t>, bool> trait_results;
  mutable std::set<std::pair<uint64_t, uint64_t>> active_constraints;
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
      return true;
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
           n.kind() != NodeKind::index_place;
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
    if (t.kind == TypeKind::Vec) {
      need(t.type_args.size() == 1, "Vec element arity");
      valid_type(t.type_args[0], allowed, depth + 1);
    }
    if (t.kind == TypeKind::Function) {
      if (t.throws_type)
        valid_type(*t.throws_type, allowed, depth + 1);
      need(type(t.result).kind != TypeKind::ForeignCallable &&
               (!t.throws_type || type(*t.throws_type).kind != TypeKind::ForeignCallable),
           "internal foreign callable in function output");
      for (auto p : t.parameters) {
        need(type(p.type).kind != TypeKind::ForeignCallable &&
                 (type(p.type).kind != TypeKind::ForeignOpaque || p.mode == ParamMode::ordinary),
             "internal callable/external move parameter");
        const auto parameter_kind = type(p.type).kind;
        const bool dependent_inout =
            parameter_kind == TypeKind::TypeParameter &&
            allowed.count(type(p.type).declaration.value);
        need(p.mode == ParamMode::ordinary || p.mode == ParamMode::move ||
                 (p.mode == ParamMode::inout &&
                  (inout_scalar(parameter_kind) || dependent_inout)),
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
  // Whole local scalar bindings cannot alias each other. An inout parameter
  // is also unique because every caller is checked by this same rule. Scan
  // nested argument expressions too: f(inout x, g(x)) must not read x while
  // f's exclusive access is active, and g(inout x) must not borrow it again.
  // Call and ConstraintCall share this check after collecting their own
  // top-level inout storage; no other argument condition is relaxed here.
  void check_call_aliases(const std::vector<CallArg> &args,
                          const std::set<uint64_t> &borrowed) const {
    for (const auto &a : args) {
      if (a.mode == ParamMode::inout)
        continue;
      std::vector<HirNodeId> pending{a.expression};
      std::set<uint64_t> visited;
      while (!pending.empty()) {
        check_cancelled(stop);
        auto id = pending.back();
        pending.pop_back();
        need(visited.insert(id.value).second,
             "cycle/shared child in call argument");
        const auto &part = node(id);
        if (part.kind() == NodeKind::read || part.kind() == NodeKind::place ||
            (part.kind() == NodeKind::move &&
             std::get<Move>(part.data).binding)) {
          auto storage = part.kind() == NodeKind::read
                             ? std::get<Read>(part.data).symbol
                         : part.kind() == NodeKind::place
                             ? std::get<Place>(part.data).symbol
                             : *std::get<Move>(part.data).binding;
          need(!borrowed.contains(storage.value),
               "inout storage is also accessed by another argument");
        }
        pending.insert(pending.end(), part.children.begin(),
                       part.children.end());
      }
    }
  }
  void check_call(const HirNode &n, const Call &c) const {
    if (c.kind == CalleeKind::foreign) {
      foreign_call(n, c);
      return;
    }
    need(!c.binding && c.omitted.empty(), "foreign metadata on ordinary call");
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
    } else
      fail("unsupported callee kind");
    need(c.args.size() == sig.parameters.size() &&
             n.children.size() == c.args.size() + offset,
         "call arity/children");
    if (offset)
      need(n.children[0] == *c.callee, "callee must be evaluated first");
    std::vector<bool> seen(sig.parameters.size());
    std::set<uint64_t> borrowed;
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
        const bool dependent =
            type(p.type).kind == TypeKind::TypeParameter &&
            owner_allowed.count(type(p.type).declaration.value);
        need(argument.kind() == NodeKind::place &&
                 (inout_scalar(type(p.type).kind) || dependent),
             "inout requires a whole trivial scalar place");
        need(
            borrowed.insert(std::get<Place>(argument.data).symbol.value).second,
            "duplicate inout storage in call");
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
    check_call_aliases(c.args, borrowed);
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
      break;
    case NodeKind::read:
    case NodeKind::place:
      arity(0);
      break; // binding checks after tree traversal
    case NodeKind::function_value: {
      arity(0);
      const auto &v = std::get<FunctionValue>(n.data);
      for (auto argument : v.type_args)
        valid_type(argument, allowed(scope(n.scope).function));
      if (!templates.count(scope(n.scope).function.value))
        need(!templates.count(v.symbol.value) && v.type_args.empty(),
             "template function value survived into concrete execution");
      need(n.type == instantiated_signature(v.symbol, v.type_args),
           "function value type mismatch");
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
               lhs.kind() == NodeKind::index_place,
           "assignment requires writable place");
      auto op = std::get<Assign>(n.data).compound;
      if (op) {
        need(*op <= BinaryOp::bit_xor, "invalid compound assignment operator");
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
    case NodeKind::integer_convert:
      arity(1);
      need(integer(type(n.type).kind) && integer(type(expr(ch[0]).type).kind),
           "integer conversion types");
      break;
    case NodeKind::call:
      check_call(n, std::get<Call>(n.data));
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
      need(type(n.type).kind != TypeKind::ForeignOpaque, "external move is unsupported");
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
    case NodeKind::index_place:
      arity(2);
      need(type(expr(ch[0]).type).kind == TypeKind::Vec &&
               integer(type(expr(ch[1]).type).kind),
           "Vec index types");
      need(n.type == type(node(ch[0]).type).type_args[0],
           "Vec indexed element type");
      break;
    case NodeKind::vec_slice: {
      const auto &sl = std::get<VecSlice>(n.data);
      need(ch.size() == 1 + bool(sl.start) + bool(sl.end), "slice arity");
      need(type(n.type).kind == TypeKind::Vec && expr(ch[0]).type == n.type,
           "slice receiver type");
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
      need(type(expr(ch[0]).type).kind == TypeKind::Vec && fi.body == ch[1] &&
               node(fi.body).kind() == NodeKind::block &&
               node(fi.body).scope == fi.body_scope,
           "for-in type/body");
      declaration(fi.element, n.source, modules::SymbolKind::value,
                  AstKind::ForIn);
      break;
    }
    case NodeKind::foreign_read:
      foreign_read(id);
      break;
    case NodeKind::unsupported:
      fail("unsupported HIR node: " + std::get<Unsupported>(n.data).feature,
           n.source.span);
    }
  }
  static bool same_ref(const modules::NodeRef &a, const modules::NodeRef &b) {
    return a.module == b.module && a.file == b.file && a.node == b.node;
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
    if (n.kind == AstKind::ParenthesizedType)
      return source_type(cr(n.children.at(0)));
    if (n.kind == AstKind::FunctionType)
      return source_signature(r);
    need(n.kind == AstKind::NamedType, "invalid type syntax source");
    std::optional<SymbolId> sid;
    for (const auto &o : m.resolved->occurrences)
      if (same_ref(o.source, r))
        sid = o.symbol;
    need(sid.has_value(), "type syntax has no resolved symbol");
    const auto &s = symbol(*sid);
    Type t;
    if (s.kind == modules::SymbolKind::type_parameter) {
      for (const auto &im : m.impls)
        if (im.self_parameter == *sid)
          return im.self_type;
      t.kind = TypeKind::TypeParameter;
      t.declaration = *sid;
    } else if (s.kind == modules::SymbolKind::type) {
      if (auto ex = m.resolved->foreign_export(*sid)) {
        need(ex->kind == modules::ForeignExportKind::type && ex->declaration && n.children.empty(),
             "foreign source type identity/arity");
        Type ft; ft.kind = TypeKind::ForeignOpaque; ft.binding = *ex->declaration;
        return substituted.intern(ft);
      }
      auto it = nominals.find(sid->value);
      need(it != nominals.end(), "type syntax nominal declaration");
      t.kind = it->second->kind;
      t.declaration = *sid;
      for (auto c : n.children)
        t.type_args.push_back(source_type(cr(c)));
    } else {
      need(s.kind == modules::SymbolKind::builtin_type,
           "type syntax symbol kind");
      if (s.name == "Vec") {
        t.kind = TypeKind::Vec;
        for (auto c : n.children)
          t.type_args.push_back(source_type(cr(c)));
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
    t.result = substituted.intern(u);
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

  bool matches(TypeId pattern, TypeId actual, Substitutions &sub) const {
    const auto &p = type(pattern), &a = type(actual);
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
  bool satisfies(TypeId actual, SymbolId trait, size_t depth = 0) const {
    auto key = std::make_pair(actual.value, trait.value);
    if (auto it = trait_results.find(key); it != trait_results.end())
      return it->second;
    if (depth >= 64 || !active_constraints.insert(key).second)
      return false;
    const auto &a = type(actual);
    size_t count = 0;
    if (a.kind == TypeKind::TypeParameter) {
      auto contains = [&](const auto &ps) {
        for (auto p : ps)
          if (p.symbol == a.declaration && p.trait == trait)
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
      for (const auto &tr : m.traits)
        if (tr.self == a.declaration && tr.symbol == trait)
          count = 1;
    } else
      for (const auto &im : m.impls)
        if (im.trait == trait) {
          Substitutions sub;
          if (!matches(im.self_type, actual, sub))
            continue;
          bool valid = true;
          for (auto p : im.params) {
            auto it = std::find_if(sub.begin(), sub.end(),
                                   [&](auto x) { return x.first == p.symbol; });
            if (it == sub.end() ||
                (p.trait && !satisfies(it->second, *p.trait, depth + 1))) {
              valid = false;
              break;
            }
          }
          if (valid)
            ++count;
        }
    active_constraints.erase(key);
    return trait_results[key] = count == 1;
  }
  void constraints(const std::vector<GenericParam> &ps,
                   const std::vector<TypeId> &args) const {
    need(ps.size() == args.size(), "constraint type argument count");
    for (size_t i = 0; i < ps.size(); ++i)
      if (ps[i].trait)
        need(satisfies(args[i], *ps[i].trait),
             "type arguments do not satisfy declared constraints");
  }
  void finite_layouts() const {
    need(!infinite_value_layout(m, stop), "infinite recursive value layout");
  }

  std::set<uint64_t> parameter_set(const std::vector<GenericParam> &ps) const {
    std::set<uint64_t> result;
    for (const auto &p : ps) {
      declaration(p.symbol, p.source, modules::SymbolKind::type_parameter,
                  AstKind::GenericParameter);
      need(result.insert(p.symbol.value).second, "duplicate generic parameter");
      if (p.trait)
        need(symbol(*p.trait).kind == modules::SymbolKind::trait,
             "constraint trait identity");
      const auto &file = *m.resolved->project->module(p.source.node.module)
                              .files.at(p.source.node.file);
      const auto &syntax = file.node(p.source.node.node);
      std::optional<SymbolId> declared;
      if (!syntax.children.empty()) {
        auto ref = p.source.node;
        ref.node = syntax.children.at(0);
        for (const auto &o : m.resolved->occurrences)
          if (same_ref(o.source, ref))
            declared = o.symbol;
        need(declared.has_value(), "constraint source has no resolved trait");
      }
      need(declared == p.trait,
           "template constraint differs from source declaration");
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
      std::set<uint64_t> ps{tr.self.value}, methods;
      for (const auto &method : tr.methods) {
        declaration(method.symbol, method.source, modules::SymbolKind::function,
                    AstKind::Function);
        need(methods.insert(method.symbol.value).second,
             "duplicate trait method");
        valid_type(method.signature, ps);
        need(method.signature == source_signature(method.source.node),
             "trait signature differs from source prototype");
        const auto &sig = type(method.signature);
        need(sig.kind == TypeKind::Function &&
                 sig.parameters.size() == method.parameters.size(),
             "trait prototype signature");
        for (const auto &f : m.functions)
          need(f.symbol != method.symbol,
               "trait prototype has fake executable body");
        for (const auto &t : m.templates)
          need(t.function.symbol != method.symbol,
               "trait prototype has fake template body");
      }
    }
    for (size_t i = 0; i < m.impls.size(); ++i)
      for (size_t j = 0; j < i; ++j)
        need(m.impls[i].trait != m.impls[j].trait ||
                 !type_patterns_overlap(m.impls[i].self_type,
                                        m.impls[j].self_type, m.types),
             "duplicate or overlapping impl patterns");
    for (const auto &im : m.impls) {
      source(im.source);
      self_owner(im.source, im.self_parameter);
      auto ps = parameter_set(im.params);
      valid_type(im.self_type, ps);
      need(type(im.self_type).kind == TypeKind::NominalStruct ||
               type(im.self_type).kind == TypeKind::NominalEnum ||
               number(type(im.self_type).kind) ||
               type(im.self_type).kind == TypeKind::Bool,
           "impl self type is not an implemented value type");
      if (im.trait)
        need(traits.count(im.trait->value), "impl trait missing");
    }
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
    if (p.binding) {
      need(!p.variant && p.payload.empty(), "binding pattern payload");
      declaration(*p.binding, p.source, modules::SymbolKind::value,
                  AstKind::BindPattern);
      need(locals.emplace(p.binding->value, Local{p.type, s, f, false, ready})
               .second,
           "duplicate pattern binding identity");
    }
    for (const auto &c : p.payload)
      register_pattern(c, s, f, ready);
  }
  bool total_pattern(const Pattern &p) const { return !p.variant; }
  void pattern_shape(const Pattern &p, TypeId expected) const {
    source(p.source);
    need(p.type == expected, "pattern type mismatch");
    if (!p.variant) {
      need(p.payload.empty(), "wildcard/binding payload");
      return;
    }
    need(!p.binding && type(expected).kind == TypeKind::NominalEnum,
         "variant pattern type");
    const VariantDecl *variant = nullptr;
    for (const auto &v : nominal_type(expected).variants)
      if (v.symbol == *p.variant)
        variant = &v;
    need(variant && p.payload.size() == variant->payload.size(),
         "pattern variant/payload mismatch");
    for (size_t i = 0; i < p.payload.size(); ++i)
      pattern_shape(p.payload[i], field_type(expected, variant->payload[i]));
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
      if (n.kind() == NodeKind::field_read)
        expr(n.children[0]);
      else
        need(base.kind() == NodeKind::place ||
                 base.kind() == NodeKind::field_place,
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
      need(type(scrutinee).kind == TypeKind::NominalEnum,
           "switch enum scrutinee");
      std::set<uint64_t> covered;
      bool wildcard = false;
      for (size_t i = 0; i < s->arms.size(); ++i) {
        const auto &a = s->arms[i];
        source(a.source);
        pattern_shape(a.pattern, scrutinee);
        need(!wildcard, "unreachable arm after wildcard");
        need(same_ref(scope(a.scope).source.node, a.source.node) &&
                 same_ref(node(a.body).source.node, a.source.node),
             "switch arm source belongs to another case");
        need(a.pattern.source.node.module == a.source.node.module &&
                 a.pattern.source.node.file == a.source.node.file &&
                 a.pattern.source.span.begin_byte >= a.source.span.begin_byte &&
                 a.pattern.source.span.end_byte <= a.source.span.end_byte,
             "pattern belongs to another arm");
        need(a.body == n.children[i + 1] &&
                 node(a.body).kind() == NodeKind::block &&
                 node(a.body).scope == a.scope &&
                 scope(a.scope).parent == n.scope,
             "switch arm body/scope");
        if (a.pattern.variant) {
          need(!covered.count(a.pattern.variant->value),
               "unreachable duplicate enum arm");
          bool total = true;
          for (const auto &p : a.pattern.payload)
            total &= total_pattern(p);
          if (total)
            covered.insert(a.pattern.variant->value);
        } else
          wildcard = true;
      }
      need(s->exhaustive &&
               (wildcard ||
                covered.size() == nominal_type(scrutinee).variants.size()),
           "nonexhaustive enum switch");
      return;
    }
    const auto &c = std::get<ConstraintCall>(n.data);
    auto owner = scope(n.scope).function;
    need(templates.count(owner.value),
         "constraint call survived into concrete execution");
    const TraitDecl *tr = nullptr;
    for (const auto &t : m.traits)
      if (t.symbol == c.trait)
        tr = &t;
    need(tr, "missing constraint trait");
    bool bounded = false;
    for (auto p : templates.at(owner.value)->params)
      if (p.trait == c.trait &&
          type(c.receiver).kind == TypeKind::TypeParameter &&
          type(c.receiver).declaration == p.symbol)
        bounded = true;
    need(bounded, "method is not authorized by template constraints");
    const MethodPrototype *method = nullptr;
    for (const auto &p : tr->methods)
      if (p.symbol == c.method)
        method = &p;
    need(method, "missing constraint method");
    const auto &prototype = type(method->signature);
    need(!method->parameters.empty() && !prototype.parameters.empty() &&
             prototype.parameters.front().mode == ParamMode::ordinary,
         "constraint method must declare ordinary self");
    // The literal name `self` is a restriction of the newly opened non-self
    // move/inout mode, not of ordinary trait calls. Existing all-ordinary
    // prototypes may name their first Self parameter `owner` (or any other
    // legal label) and must keep their old behavior.
    const bool non_self_modes =
        std::any_of(prototype.parameters.begin() + 1,
                    prototype.parameters.end(), [](const auto &p) {
                      return p.mode != ParamMode::ordinary;
                    });
    if (non_self_modes)
      need(method->parameters.front().label == "self",
           "constraint move/inout mode requires ordinary self");
    const auto &raw_self = type(prototype.parameters.front().type);
    need(raw_self.kind == TypeKind::TypeParameter &&
             raw_self.declaration == tr->self,
         "constraint method self type must be the trait Self");
    need(c.signature == substitute(method->signature, {{tr->self, c.receiver}},
                                   substituted),
         "constraint method substitution");
    const auto &sig = type(c.signature);
    need(n.type == sig.result && c.args.size() == sig.parameters.size() &&
             n.children.size() == c.args.size() && !c.args.empty(),
         "constraint call shape");
    // The receiver is a synthetic first argument. It must be evaluated first,
    // once, as an ordinary read; only explicit arguments follow in source
    // order. Labels come from the real trait prototype, not a name match.
    const auto &receiver = c.args[0];
    need(receiver.source_index == 0 && receiver.parameter_index == 0 &&
             !receiver.label && receiver.mode == ParamMode::ordinary &&
             receiver.expression == n.children[0] &&
             expr(receiver.expression).type == c.receiver,
         "constraint receiver order/mode/type");
    std::set<uint32_t> seen{0};
    std::set<uint64_t> borrowed;
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
        // Trait prototypes have no method-own type parameters. Requiring a
        // concrete scalar here also keeps `slot inout Self` unavailable even
        // if a concrete impl would otherwise make its data scalar.
        need(argument.kind() == NodeKind::place &&
                 inout_scalar(type(p.type).kind),
             "constraint inout requires a whole trivial scalar place");
        need(borrowed
                 .insert(std::get<Place>(argument.data).symbol.value)
                 .second,
             "duplicate inout storage in constraint call");
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
    check_call_aliases(c.args, borrowed);
  }
  void instance_body(const HirFunction &origin, const HirFunction &f,
                     const Substitutions &sub,
                     const std::map<uint64_t, SymbolId> &renamed) const {
    std::map<uint64_t, HirNodeId> nodes;
    std::map<uint64_t, ScopeId> scopes;
    std::set<uint64_t> destinations;
    std::vector<std::pair<HirNodeId, HirNodeId>> work{{origin.body, f.body}};
    while (!work.empty()) {
      check_cancelled(stop);
      auto [a, b] = work.back();
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
      need(x.kind() == y.kind() || (x.kind() == NodeKind::constraint_call &&
                                    y.kind() == NodeKind::call),
           "instance node kind differs from template");
      for (size_t i = 0; i < x.children.size(); ++i)
        work.emplace_back(x.children[i], y.children[i]);
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
        const ImplDecl *impl = nullptr;
        for (const auto &im : m.impls) {
          if (im.trait != cc->trait)
            continue;
          Substitutions match;
          if (!matches(im.self_type, receiver, match))
            continue;
          need(!impl, "ambiguous instance constraint impl");
          impl = &im;
        }
        need(impl, "instance constraint call has no impl");
        const MethodPrototype *prototype = nullptr;
        for (const auto &tr : m.traits)
          if (tr.symbol == cc->trait)
            for (const auto &p : tr.methods)
              if (p.symbol == cc->method) {
                need(!prototype, "duplicate instance constraint method");
                prototype = &p;
              }
        need(prototype, "instance constraint prototype missing");
        std::optional<SymbolId> expected;
        for (auto method : impl->methods)
          if (symbol(method).name == symbol(prototype->symbol).name) {
            need(!expected, "duplicate instance impl method");
            expected = method;
          }
        need(expected.has_value(), "instance constraint impl method missing");
        auto target = *actual->direct_symbol;
        for (const auto &i : m.instances)
          if (i.symbol == target) {
            target = i.origin;
            break;
          }
        need(target == *expected,
             "instance constraint target differs from selected trait impl");
        for (size_t a = 0; a < cc->args.size(); ++a) {
          const auto &before = cc->args[a], &after = actual->args[a];
          need(before.source_index == after.source_index &&
                   before.parameter_index == after.parameter_index &&
                   before.label == after.label && before.mode == after.mode &&
                   after.expression == y.children[a],
               "instance constraint argument mapping/mode/order");
        }
      }
      if (auto r = std::get_if<Read>(&x.data))
        need(std::get<Read>(y.data).symbol == local(r->symbol),
             "instance read binding mismatch");
      if (auto r = std::get_if<Place>(&x.data))
        need(std::get<Place>(y.data).symbol == local(r->symbol),
             "instance place binding mismatch");
      if (auto r = std::get_if<Binding>(&x.data))
        need(std::get<Binding>(y.data).symbol == local(r->symbol),
             "instance declaration binding mismatch");
      if (auto r = std::get_if<Move>(&x.data))
        need(
            std::get<Move>(y.data).binding ==
                (r->binding ? std::optional(local(*r->binding)) : std::nullopt),
            "instance move source mismatch");
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
        need(originals.count(from.value) && a.definition &&
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
        const TraitDecl *tr = nullptr;
        for (const auto &t : m.traits)
          if (t.symbol == *im.trait)
            tr = &t;
        need(tr && im.methods.size() == tr->methods.size(),
             "impl method count");
        for (const auto &p : tr->methods) {
          const HirFunction *f = nullptr;
          for (auto id : im.methods)
            if (symbol(id).name == symbol(p.symbol).name) {
              need(!f, "duplicate impl method");
              f = &function(id);
            }
          need(f && f->signature == substitute(p.signature,
                                               {{tr->self, im.self_type}},
                                               substituted),
               "impl method signature mismatch");
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
            n.kind() == NodeKind::index_place) {
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
                          Local{type(node(n.children[0]).type).type_args[0],
                                fi->body_scope, f.symbol, false,
                                enter[id.value]})
                 .second,
             "duplicate for-in element");
      if (n.kind() == NodeKind::return_) {
        need(std::get<Return>(n.data).function == f.symbol,
             "return target function mismatch");
        const auto &sig = type(f.signature);
        need(n.children.empty() ? type(sig.result).kind == TypeKind::Unit
                                : expr(n.children[0]).type == sig.result,
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
              ex && (ex->kind == modules::ForeignExportKind::value ||
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
      if (n.kind() == NodeKind::foreign_read)
        actual_foreign_reads.insert(unwrapped(n.source.node).node.value);
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
                 (kind == AstKind::Slice && n.kind() == NodeKind::vec_slice) ||
                 (kind == AstKind::ForIn && n.kind() == NodeKind::for_in) ||
                 (kind == AstKind::Unary && n.kind() == NodeKind::move) ||
                 (index_syntax(file.node(n.source.node.node)) &&
                  (n.kind() == NodeKind::index_read ||
                   n.kind() == NodeKind::index_place)),
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
                  classify_value(m, n.type).needs_drop;
    if (expected.empty() && !owning)
      return; // preserve existing scalar builder lowering
    const auto &syntax_function = file.node(ref.node);
    need(!syntax_function.children.empty(), "function has no source body");
    auto exact_child = [&](HirNodeId child, AstNodeId syntax) {
      auto expected_ref = ref;
      expected_ref.node = syntax;
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

  void ownership_sources() const {
    std::set<uint64_t> owning_functions;
    for (const auto &n : m.nodes)
      if (n.kind() == NodeKind::move || n.kind() == NodeKind::for_in ||
          classify_value(m, n.type).needs_drop)
        owning_functions.insert(scope(n.scope).function.value);
    auto origin = [&](SymbolId id) {
      for (const auto &i : m.instances)
        for (auto [from, to] : i.renamed)
          if (to == id)
            return from;
      return id;
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
      std::optional<SymbolId> read;
      if (auto p = std::get_if<Read>(&n.data);
          p && classify_value(m, n.type).needs_drop)
        read = p->symbol;
      if (auto mv = std::get_if<Move>(&n.data)) {
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
      if (read) {
        unwrap();
        std::optional<SymbolId> actual;
        for (const auto &o : m.resolved->occurrences)
          if (same_ref(o.source, r))
            actual = o.symbol;
        need(actual && *actual == origin(*read),
             "owning read/move source identity differs from resolver");
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
          need(type(function(owner).signature).throws_type == error,
               "unhandled typed error not declared by function");
          exit(ExitKind::error, FunctionExit{owner}, {}, error, true);
        }
      }
      if (n.kind() == NodeKind::return_) {
        need(!enclosing_defer, "return crosses defer boundary");
        exit(ExitKind::return_, FunctionExit{owner}, {}, {},
             !n.children.empty());
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
    for (const auto *fp : all_functions)
      need(type(type(fp->signature).result).kind == TypeKind::Unit ||
               !flows[fp->body.value].falls,
           "non-Unit function has a normal exit without return");
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
    finite_layouts();
    for (auto owner : owners)
      need(owner.has_value(), "orphan executable node");
    for (bool used : scope_used)
      need(used, "orphan scope");
    uses_and_flow();
    ownership_sources();
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
    TypeContext storage_types = m.types;
    std::set<uint64_t> checked_types;
    std::function<bool(TypeId)> supported = [&](TypeId id) {
      check_cancelled(stop);
      const auto t = *storage_types.find(id);
      if (t.kind == TypeKind::ForeignOpaque || t.kind == TypeKind::ForeignCallable)
        return foreign_values;
      if (!checked_types.insert(id.value).second)
        return true;
      if (t.kind == TypeKind::Vec) {
        if (!ownership_values)
          return false;
        const auto &element = *storage_types.find(t.type_args.at(0));
        if (element.kind == TypeKind::ForeignOpaque ||
            element.kind == TypeKind::ForeignCallable)
          return false;
        return supported(t.type_args.at(0));
      }
      if (t.kind == TypeKind::NominalStruct ||
          t.kind == TypeKind::NominalEnum) {
        const auto d = std::find_if(
            m.nominals.begin(), m.nominals.end(),
            [&](const auto &d) { return d.symbol == t.declaration; });
        if (d == m.nominals.end())
          return false;
        Substitutions subst;
        for (size_t p = 0; p < d->params.size(); ++p)
          subst.emplace_back(d->params[p].symbol, t.type_args[p]);
        auto fields = [&](const auto &fs) {
          for (const auto &f : fs) {
            auto field = substitute(f.type, subst, storage_types);
            const auto kind = storage_types.find(field)->kind;
            if (kind == TypeKind::String ||
                kind == TypeKind::ForeignOpaque ||
                kind == TypeKind::ForeignCallable || !supported(field))
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
          auto current = pending.back();
          pending.pop_back();
          if (!seen.insert(current.value).second)
            continue;
          const auto value = *storage_types.find(current);
          if (value.kind == TypeKind::ForeignOpaque ||
              value.kind == TypeKind::ForeignCallable) {
            if (!foreign_values)
              return false;
            continue;
          }
          if (value.kind == TypeKind::Vec) {
            if (!ownership_values)
              return false;
            const auto &element = *storage_types.find(value.type_args.at(0));
            if (element.kind == TypeKind::ForeignOpaque ||
                element.kind == TypeKind::ForeignCallable)
              return false;
          }
          if (value.kind == TypeKind::NominalStruct ||
              value.kind == TypeKind::NominalEnum) {
            for (const auto &d : m.nominals)
              if (d.symbol == value.declaration) {
                auto add = [&](TypeId field) {
                  const auto kind = storage_types.find(field)->kind;
                  if (kind == TypeKind::ForeignOpaque ||
                      kind == TypeKind::ForeignCallable)
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
      return t.kind != TypeKind::Vec && t.kind != TypeKind::TypeParameter &&
             t.kind != TypeKind::Error && t.kind != TypeKind::ForeignOpaque &&
             t.kind != TypeKind::ForeignCallable;
    };
    auto signature_supported = [&](TypeId id) {
      const auto t = *m.types.find(id);
      if (t.throws_type && !supported(*t.throws_type))
        return false;
      for (auto p : t.parameters)
        if ((p.mode == ParamMode::move && !ownership_values) ||
            !supported(p.type))
          return false;
      return supported(t.result);
    };
    for (const auto &f : m.functions) {
      check_cancelled(stop);
      if (!signature_supported(f.signature)) {
        unavailable(
            f.source,
            "backend does not support this concrete function signature");
        return r;
      }
      std::vector<HirNodeId> todo{f.body};
      while (!todo.empty()) {
        check_cancelled(stop);
        auto id = todo.back();
        todo.pop_back();
        const auto &n = m.nodes[id.value];
        if (n.kind() == NodeKind::foreign_read ||
            (std::holds_alternative<Call>(n.data) &&
             std::get<Call>(n.data).kind == CalleeKind::foreign)) {
          if (!foreign_values) {
            unavailable(n.source,
                        "foreign reads/calls require the Python/64 profile");
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
        }
        switch (n.kind()) {
        case NodeKind::struct_init:
        case NodeKind::enum_init:
        case NodeKind::field_read:
        case NodeKind::field_place:
        case NodeKind::switch_:
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
        if (!supported(n.type)) {
          unavailable(n.source,
                      "backend does not support this concrete aggregate type");
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
  auto params = [&](const auto &ps) {
    for (const auto &p : ps) {
      s << " generic=" << p.symbol.value << ':';
      if (p.trait)
        s << p.trait->value;
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
      s << im.trait->value;
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
          else if constexpr (std::is_same_v<T, Read> ||
                             std::is_same_v<T, Place> ||
                             std::is_same_v<T, FunctionValue>) {
            s << " symbol=" << data.symbol.value;
            if constexpr (std::is_same_v<T, FunctionValue>)
              for (auto t : data.type_args)
                s << " type_arg=" << t.value;
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
            s << " trait=" << data.trait.value
              << " method=" << data.method.value
              << " receiver=" << data.receiver.value
              << " signature=" << data.signature.value;
            for (const auto &a : data.args)
              s << " arg=" << a.source_index << ':' << a.parameter_index << ':'
                << a.expression.value;
          } else if constexpr (std::is_same_v<T, ForeignRead>) {
            s << " foreign_read=" << data.binding.value;
          } else if constexpr (std::is_same_v<T, Call>) {
            for (auto t : data.type_args)
              s << " type_arg=" << t.value;
            if (data.binding) s << " binding=" << data.binding->value;
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
  for (auto l : m.ownership.locals)
    s << "lifetime " << l.symbol.value << " scope=" << l.scope.value
      << " type=" << l.type.value << " order=" << l.order
      << " mode=" << int(l.mode) << '\n';
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
