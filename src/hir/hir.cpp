#include "lang/hir/hir.hpp"
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
  if (!nominal(t.kind) && t.kind != TypeKind::ForeignOpaque &&
      (t.declaration.value || t.binding.value))
    bad();
  if (nominal(t.kind) && t.binding.value)
    bad();
  if (t.kind != TypeKind::NominalStruct && t.kind != TypeKind::NominalEnum &&
      !t.type_args.empty())
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
      if (!seen.insert(id.value).second) continue;
      auto t = m.types.find(id);
      if (!t) throw std::invalid_argument("layout has invalid type");
      if (t->kind == TypeKind::TypeParameter) visit(*t);
      if (t->kind != TypeKind::NominalStruct && t->kind != TypeKind::NominalEnum)
        continue;
      visit(*t);
      for (auto i : used.at(t->declaration.value))
        pending.push_back(t->type_args.at(i));
    }
  };
  auto fields = [&](const NominalDecl &d, const auto &visit) {
    for (const auto &f : d.fields) walk(f.type, visit);
    for (const auto &v : d.variants)
      for (const auto &f : v.payload) walk(f.type, visit);
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
    for (auto target : deps) dependents[target].insert(d.symbol.value);
    if (deps.empty()) leaves.push_back(d.symbol.value);
  }
  while (!leaves.empty()) {
    check_cancelled(stop);
    auto finite = leaves.back();
    leaves.pop_back();
    for (auto parent : dependents[finite]) {
      auto &deps = dependencies.at(parent);
      deps.erase(finite);
      if (deps.empty()) leaves.push_back(parent);
    }
  }
  for (const auto &d : m.nominals)
    if (!dependencies.at(d.symbol.value).empty()) return d.symbol;
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
           n.kind() != NodeKind::field_place;
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
    need(t.kind != TypeKind::Error && t.kind != TypeKind::ForeignOpaque,
         "invalid/unimplemented type");
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
    if (t.kind == TypeKind::Function) {
      need(!t.throws_type, "throws execution is unsupported");
      for (auto p : t.parameters) {
        need(p.mode == ParamMode::ordinary || (p.mode == ParamMode::inout &&
                                               inout_scalar(type(p.type).kind)),
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
  void check_call(const HirNode &n, const Call &c) const {
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
        need(argument.kind() == NodeKind::place &&
                 inout_scalar(type(p.type).kind),
             "inout requires a whole trivial scalar place");
        need(
            borrowed.insert(std::get<Place>(argument.data).symbol.value).second,
            "duplicate inout storage in call");
      } else {
        need(p.mode == ParamMode::ordinary, "unsupported argument mode");
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
    // Whole local scalar bindings cannot alias each other. An inout parameter
    // is also unique because every caller is checked by this same rule. Scan
    // nested argument expressions too: f(inout x, g(x)) must not read x while
    // f's exclusive access is active, and g(inout x) must not borrow it again.
    for (const auto &a : c.args) {
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
        if (part.kind() == NodeKind::read || part.kind() == NodeKind::place) {
          auto storage = part.kind() == NodeKind::read
                             ? std::get<Read>(part.data).symbol
                             : std::get<Place>(part.data).symbol;
          need(!borrowed.contains(storage.value),
               "inout storage is also accessed by another argument");
        }
        pending.insert(pending.end(), part.children.begin(),
                       part.children.end());
      }
    }
  }
  void shape(HirNodeId id) const {
    check_cancelled(stop);
    const auto &n = node(id);
    need(!n.data.valueless_by_exception(), "valueless HIR payload");
    source(n.source);
    valid_type(n.type, allowed(scope(n.scope).function));
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
      need(lhs.kind() == NodeKind::place || lhs.kind() == NodeKind::field_place,
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
      auto it = nominals.find(sid->value);
      need(it != nominals.end(), "type syntax nominal declaration");
      t.kind = it->second->kind;
      t.declaration = *sid;
      for (auto c : n.children)
        t.type_args.push_back(source_type(cr(c)));
    } else {
      need(s.kind == modules::SymbolKind::builtin_type,
           "type syntax symbol kind");
      if (s.name == "Unit")
        t.kind = TypeKind::Unit;
      else if (s.name == "bool")
        t.kind = TypeKind::Bool;
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
    for (auto c : file.node(r.node).children) {
      auto cr = r;
      cr.node = c;
      const auto &n = file.node(c);
      if (n.kind == AstKind::Parameter) {
        auto mode = ParamMode::ordinary;
        for (size_t i = n.token_begin;
             i < file.node(n.children.at(0)).token_begin; ++i) {
          if (file.tokens[i].spelling == "inout")
            mode = ParamMode::inout;
          if (file.tokens[i].spelling == "move")
            mode = ParamMode::move;
        }
        cr.node = n.children.at(0);
        t.parameters.push_back({mode, source_type(cr)});
      } else if (n.kind == AstKind::NamedType ||
                 n.kind == AstKind::FunctionType ||
                 n.kind == AstKind::ParenthesizedType)
        t.result = source_type(cr);
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
    need(expected.size() == entries.size(), "declaration member count differs from source");
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
      std::set<uint32_t> seen;
      for (size_t i = 0; i < args->size(); ++i) {
        const auto &a = (*args)[i];
        need(a.source_index == i && a.parameter_index < fields->size() &&
                 seen.insert(a.parameter_index).second,
             "constructor argument mapping");
        const auto &f = (*fields)[a.parameter_index];
        need(n.children[i] == a.expression && a.mode == ParamMode::ordinary &&
                 a.label && *a.label == symbol(f.symbol).name &&
                 expr(a.expression).type == field_type(n.type, f),
             "constructor argument type/label/order");
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
    need(c.signature == substitute(method->signature, {{tr->self, c.receiver}},
                                   substituted),
         "constraint method substitution");
    const auto &sig = type(c.signature);
    need(n.type == sig.result && c.args.size() == sig.parameters.size() &&
             n.children.size() == c.args.size(),
         "constraint call shape");
    std::set<uint32_t> seen;
    for (size_t i = 0; i < c.args.size(); ++i) {
      const auto &a = c.args[i];
      need(a.source_index == i && a.parameter_index < sig.parameters.size() &&
               seen.insert(a.parameter_index).second,
           "constraint call argument mapping");
      need(n.children[i] == a.expression &&
               expr(a.expression).type ==
                   sig.parameters[a.parameter_index].type &&
               a.mode == sig.parameters[a.parameter_index].mode,
           "constraint argument type/order");
    }
  }
  void instance_records() {
    std::set<uint64_t> generated;
    // Resolver declarations precede generated copies. Retain their actual
    // SymbolIds so an instance copy with the same AST/name is not an origin.
    std::map<std::tuple<uint64_t, size_t, uint64_t>, SymbolId> original_declarations;
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
      const auto &file = *m.resolved->project->module(ref.module).files[ref.file];
      auto &originals = template_symbols[t.function.symbol.value];
      std::vector<AstNodeId> pending{ref.node};
      while (!pending.empty()) {
        check_cancelled(stop);
        auto id = pending.back();
        pending.pop_back();
        const auto &n = file.node(id);
        // Function-type parameters have no variable declaration symbols.
        if (n.kind == AstKind::FunctionType) continue;
        if (id != ref.node && (n.kind == AstKind::Function ||
                               n.kind == AstKind::FunctionLiteral)) continue;
        if (id == ref.node || n.kind == AstKind::Parameter ||
            n.kind == AstKind::Binding ||
            (n.kind == AstKind::BindPattern &&
             file.tokens[n.token_begin].spelling != "_"))
          originals.insert(original_declarations.at(std::make_tuple(
              ref.module.value, ref.file, id.value)).value);
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
          need(originals.count(sid), "template local is not an original symbol");
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
        if (n.kind() == NodeKind::block || n.kind() == NodeKind::for_)
          need(s.parent && *s.parent == p.scope,
               "nested scope must have structural parent");
        else
          need(n.scope == p.scope,
               "node scope differs from enclosing construct");
        if (n.kind() == NodeKind::place || n.kind() == NodeKind::field_place) {
          bool allowed = p.kind() == NodeKind::assign && p.children[0] == id;
          if (p.kind() == NodeKind::call)
            for (const auto &a : std::get<Call>(p.data).args)
              allowed =
                  allowed || (a.mode == ParamMode::inout && a.expression == id);
          allowed = allowed ||
                    (p.kind() == NodeKind::field_place && p.children[0] == id);
          need(allowed, "place used outside assignment or inout argument");
        }
      }
      if (n.kind() == NodeKind::block || n.kind() == NodeKind::for_) {
        need(!scope_used[n.scope.value],
             "scope has multiple owning constructs");
        scope_used[n.scope.value] = true;
      }
      if (n.kind() == NodeKind::for_)
        loops.push_back(id);
      if (n.kind() == NodeKind::for_ || n.kind() == NodeKind::switch_)
        break_targets.push_back(id);
      if (n.kind() == NodeKind::switch_) {
        for (const auto &a : std::get<Switch>(n.data).arms)
          register_pattern(a.pattern, a.scope, f.symbol, enter[id.value]);
      }
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
      if (n.kind() == NodeKind::for_)
        loops.pop_back();
      if (n.kind() == NodeKind::for_ || n.kind() == NodeKind::switch_)
        break_targets.pop_back();
      stack.pop_back();
    }
  }
  void uses_and_flow() const {
    std::vector<bool> falls(m.nodes.size(), true);
    std::vector<std::set<uint64_t>> reachable_breaks(m.nodes.size());
    for (size_t i = 0; i < m.nodes.size(); ++i) {
      check_cancelled(stop);
      const auto &n = m.nodes[i];
      if (n.kind() != NodeKind::read && n.kind() != NodeKind::place)
        continue;
      auto s = n.kind() == NodeKind::read ? std::get<Read>(n.data).symbol
                                          : std::get<Place>(n.data).symbol;
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
    for (auto i : order) {
      check_cancelled(stop);
      const auto &n = m.nodes[i];
      switch (n.kind()) {
      case NodeKind::break_:
        reachable_breaks[i].insert(std::get<Break>(n.data).loop.value);
        falls[i] = false;
        break;
      case NodeKind::return_:
      case NodeKind::continue_:
        falls[i] = false;
        break;
      case NodeKind::block:
        for (auto c : n.children) {
          if (!falls[i])
            break;
          const auto &exits = reachable_breaks[c.value];
          reachable_breaks[i].insert(exits.begin(), exits.end());
          falls[i] = falls[c.value];
        }
        break;
      case NodeKind::if_: {
        size_t first = 1, end = n.children.size();
        const auto &condition = node(n.children[0]);
        if (condition.kind() == NodeKind::bool_literal) {
          first = std::get<BoolLiteral>(condition.data).value ? 1 : 2;
          end = std::min(first + 1, n.children.size());
          falls[i] = first == end || falls[n.children[first].value];
        } else
          falls[i] = falls[n.children[1].value] || n.children.size() == 2 ||
                     falls[n.children[2].value];
        for (size_t branch = first; branch < end; ++branch) {
          const auto &exits = reachable_breaks[n.children[branch].value];
          reachable_breaks[i].insert(exits.begin(), exits.end());
        }
        break;
      }
      case NodeKind::switch_: {
        falls[i] = false;
        for (size_t a = 1; a < n.children.size(); ++a) {
          auto ch = n.children[a].value;
          falls[i] = falls[i] || falls[ch] || reachable_breaks[ch].contains(i);
          for (auto target : reachable_breaks[ch])
            if (target != i)
              reachable_breaks[i].insert(target);
        }
        break;
      }
      case NodeKind::for_: {
        const auto &cond = node(n.children[1]);
        falls[i] = !(cond.kind() == NodeKind::bool_literal &&
                     std::get<BoolLiteral>(cond.data).value &&
                     !reachable_breaks[n.children[3].value].contains(i));
        // Breaks from this body terminate this For. They cannot leave an
        // enclosing loop because tree() requires the nearest loop target.
        break;
      }
      default:
        break;
      }
    }
    for (const auto *fp : all_functions)
      need(type(type(fp->signature).result).kind == TypeKind::Unit ||
               !falls[fp->body.value],
           "non-Unit function has a normal exit without return");
  }

public:
  Verifier(const HirModule &module, std::stop_token token)
      : m(module), stop(token), enter(m.nodes.size()), leave(m.nodes.size()),
        owners(m.nodes.size()), scope_used(m.scopes.size()),
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
    for (const auto *f : all_functions)
      tree(*f);
    instance_records();
    finite_layouts();
    for (auto owner : owners)
      need(owner.has_value(), "orphan executable node");
    for (bool used : scope_used)
      need(used, "orphan scope");
    uses_and_flow();
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
    TypeContext storage_types = m.types;
    std::set<uint64_t> checked_types;
    std::function<bool(TypeId)> supported = [&](TypeId id) {
      check_cancelled(stop);
      if (!checked_types.insert(id.value).second)
        return true;
      const auto t = *storage_types.find(id);
      if (t.kind == TypeKind::NominalStruct || t.kind == TypeKind::NominalEnum) {
        if (m.config.backend != Backend::native)
          return false;
        const auto d = std::find_if(m.nominals.begin(), m.nominals.end(),
                                   [&](const auto &d) { return d.symbol == t.declaration; });
        if (d == m.nominals.end())
          return false;
        Substitutions subst;
        for (size_t p = 0; p < d->params.size(); ++p)
          subst.emplace_back(d->params[p].symbol, t.type_args[p]);
        auto fields = [&](const auto &fs) {
          for (const auto &f : fs) {
            auto field = substitute(f.type, subst, storage_types);
            if (storage_types.find(field)->kind == TypeKind::String || !supported(field))
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
      if (m.config.backend == Backend::python && t.kind == TypeKind::Function) {
        for (auto p : t.parameters)
          if (!supported(p.type))
            return false;
        if (!supported(t.result))
          return false;
      }
      // Function values occupy one pointer. Do not expand their signatures
      // while inspecting storage (e.g. S[T].f: func() S[S[T]]).
      return t.kind != TypeKind::TypeParameter && t.kind != TypeKind::Error &&
             t.kind != TypeKind::ForeignOpaque;
    };
    auto signature_supported = [&](TypeId id) {
      const auto t = *m.types.find(id);
      if (t.throws_type)
        return false;
      for (auto p : t.parameters)
        if (!supported(p.type))
          return false;
      return supported(t.result);
    };
    for (const auto &f : m.functions) {
      check_cancelled(stop);
      if (!signature_supported(f.signature)) {
        unavailable(
            f.source,
            "backend does not support this concrete aggregate signature");
        return r;
      }
      std::vector<HirNodeId> todo{f.body};
      while (!todo.empty()) {
        check_cancelled(stop);
        auto id = todo.back();
        todo.pop_back();
        const auto &n = m.nodes[id.value];
        switch (n.kind()) {
        case NodeKind::struct_init:
        case NodeKind::enum_init:
        case NodeKind::field_read:
        case NodeKind::field_place:
        case NodeKind::switch_:
          if (m.config.backend == Backend::native)
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
        bool float_operation = false;
        if (n.kind() == NodeKind::binary || n.kind() == NodeKind::unary)
          float_operation =
              !n.children.empty() &&
              m.types.find(m.nodes[n.children[0].value].type)->kind ==
                  TypeKind::Float;
        if (auto a = std::get_if<Assign>(&n.data); a && a->compound)
          float_operation =
              m.types.find(m.nodes[n.children[0].value].type)->kind ==
              TypeKind::Float;
        if (float_operation) {
          unavailable(n.source, "strict floating point operations are not "
                                "implemented by this backend");
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
                                      std::stop_token stop) {
  PhaseResult<VerifiedHirModule> r;
  try {
    auto module = freeze(input, stop);
    auto checked = check_backend_capabilities(*module, stop);
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
          } else if constexpr (std::is_same_v<T, ConstraintCall>) {
            s << " trait=" << data.trait.value
              << " method=" << data.method.value
              << " receiver=" << data.receiver.value
              << " signature=" << data.signature.value;
            for (const auto &a : data.args)
              s << " arg=" << a.source_index << ':' << a.parameter_index << ':'
                << a.expression.value;
          } else if constexpr (std::is_same_v<T, Call>) {
            for (auto t : data.type_args)
              s << " type_arg=" << t.value;
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
    s << '\n';
  }
  return s.str();
}
std::string dump(const VerifiedHirModule &m) { return dump(m.module()); }
} // namespace lang::hir
